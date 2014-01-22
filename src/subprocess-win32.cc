// Copyright 2012 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "subprocess.h"

#include <stdint.h>
#include <stdio.h>

#include <algorithm>

#include "util.h"

Subprocess::Subprocess() : child_(NULL) , overlapped_(), is_reading_(false), pipe_(NULL),
                           use_override_status_(false), override_status_(ExitFailure) {
}

Subprocess::~Subprocess() {
  if (pipe_) {
    if (!CloseHandle(pipe_))
      Win32Fatal("CloseHandle");
  }
  // Reap child if forgotten.
  if (child_)
    Finish();
}

HANDLE Subprocess::SetupPipe(HANDLE ioport) {
  char pipe_name[100];
  snprintf(pipe_name, sizeof(pipe_name),
           "\\\\.\\pipe\\ninja_pid%lu_sp%p", GetCurrentProcessId(), this);

  pipe_ = ::CreateNamedPipeA(pipe_name,
                             PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
                             PIPE_TYPE_BYTE,
                             PIPE_UNLIMITED_INSTANCES,
                             0, 0, INFINITE, NULL);
  if (pipe_ == INVALID_HANDLE_VALUE)
    Win32Fatal("CreateNamedPipe");

  if (!CreateIoCompletionPort(pipe_, ioport, (ULONG_PTR)this, 0))
    Win32Fatal("CreateIoCompletionPort");

  memset(&overlapped_, 0, sizeof(overlapped_));
  if (!ConnectNamedPipe(pipe_, &overlapped_) &&
      GetLastError() != ERROR_IO_PENDING) {
    Win32Fatal("ConnectNamedPipe");
  }

  // Get the write end of the pipe as a handle inheritable across processes.
  HANDLE output_write_handle = CreateFile(pipe_name, GENERIC_WRITE, 0,
                                          NULL, OPEN_EXISTING, 0, NULL);
  HANDLE output_write_child;
  if (!DuplicateHandle(GetCurrentProcess(), output_write_handle,
                       GetCurrentProcess(), &output_write_child,
                       0, TRUE, DUPLICATE_SAME_ACCESS)) {
    Win32Fatal("DuplicateHandle");
  }
  CloseHandle(output_write_handle);

  return output_write_child;
}

bool Subprocess::Start(SubprocessSet* set, const string& command) {
  HANDLE child_pipe = SetupPipe(set->ioport_);

  SECURITY_ATTRIBUTES security_attributes;
  memset(&security_attributes, 0, sizeof(SECURITY_ATTRIBUTES));
  security_attributes.nLength = sizeof(SECURITY_ATTRIBUTES);
  security_attributes.bInheritHandle = TRUE;
  // Must be inheritable so subprocesses can dup to children.
  HANDLE nul = CreateFile("NUL", GENERIC_READ,
          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
          &security_attributes, OPEN_EXISTING, 0, NULL);
  if (nul == INVALID_HANDLE_VALUE)
    Fatal("couldn't open nul");

  STARTUPINFOA startup_info;
  memset(&startup_info, 0, sizeof(startup_info));
  startup_info.cb = sizeof(STARTUPINFO);
  startup_info.dwFlags = STARTF_USESTDHANDLES;
  startup_info.hStdInput = nul;
  startup_info.hStdOutput = child_pipe;
  startup_info.hStdError = child_pipe;

  PROCESS_INFORMATION process_info;
  memset(&process_info, 0, sizeof(process_info));

  // Do not prepend 'cmd /c' on Windows, this breaks command
  // lines greater than 8,191 chars.
  if (!CreateProcessA(NULL, (char*)command.c_str(), NULL, NULL,
                      /* inherit handles */ TRUE, CREATE_NEW_PROCESS_GROUP,
                      NULL, NULL,
                      &startup_info, &process_info)) {
    DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND) {
      // File (program) not found error is treated as a normal build
      // action failure.
      if (child_pipe)
        CloseHandle(child_pipe);
      CloseHandle(pipe_);
      CloseHandle(nul);
      pipe_ = NULL;
      // child_ is already NULL;
      buf_ = "CreateProcess failed: The system cannot find the file "
          "specified.\n";
      return true;
    } else {
      Win32Fatal("CreateProcess");    // pass all other errors to Win32Fatal
    }
  }

  // Close pipe channel only used by the child.
  if (child_pipe)
    CloseHandle(child_pipe);
  CloseHandle(nul);

  CloseHandle(process_info.hThread);
  child_ = process_info.hProcess;

  return true;
}

void Subprocess::OnPipeReady() {
  DWORD bytes;
  if (!GetOverlappedResult(pipe_, &overlapped_, &bytes, TRUE)) {
    if (GetLastError() == ERROR_BROKEN_PIPE) {
      CloseHandle(pipe_);
      pipe_ = NULL;
      return;
    }
    Win32Fatal("GetOverlappedResult");
  }

  if (is_reading_ && bytes)
    buf_.append(overlapped_buf_, bytes);

  memset(&overlapped_, 0, sizeof(overlapped_));
  is_reading_ = true;
  if (!::ReadFile(pipe_, overlapped_buf_, sizeof(overlapped_buf_),
                  &bytes, &overlapped_)) {
    if (GetLastError() == ERROR_BROKEN_PIPE) {
      CloseHandle(pipe_);
      pipe_ = NULL;
      return;
    }
    if (GetLastError() != ERROR_IO_PENDING)
      Win32Fatal("ReadFile");
  }

  // Even if we read any bytes in the readfile call, we'll enter this
  // function again later and get them at that point.
}

ExitStatus Subprocess::Finish() {
  if (use_override_status_) {
    return override_status_;
  }

  if (!child_)
    return ExitFailure;

  // TODO: add error handling for all of these.
  WaitForSingleObject(child_, INFINITE);

  DWORD exit_code = 0;
  GetExitCodeProcess(child_, &exit_code);

  CloseHandle(child_);
  child_ = NULL;

  return exit_code == 0              ? ExitSuccess :
         exit_code == CONTROL_C_EXIT ? ExitInterrupted :
                                       ExitFailure;
}

bool Subprocess::Done() const {
  return pipe_ == NULL;
}

const string& Subprocess::GetOutput() const {
  return buf_;
}

HANDLE SubprocessSet::ioport_;

SubprocessSet::SubprocessSet() : batch_mode_(false) {
  ioport_ = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
  if (!ioport_)
    Win32Fatal("CreateIoCompletionPort");
  if (!SetConsoleCtrlHandler(NotifyInterrupted, TRUE))
    Win32Fatal("SetConsoleCtrlHandler");
}

SubprocessSet::~SubprocessSet() {
  Clear();

  SetConsoleCtrlHandler(NotifyInterrupted, FALSE);
  CloseHandle(ioport_);
}

void SubprocessSet::SetBatchMode(bool b) {
  if (b) {
    // Try to detect if dbsrun is in the PATH.
    DWORD searchRet = SearchPath(
        NULL, "dbsrun.exe", NULL, 0, NULL, NULL);
    if (searchRet == 0) {
      batch_mode_ = false;
      return;
    }

    batch_mode_ = true;
    string userName;
    DWORD userSize = GetEnvironmentVariable("USERNAME", NULL, 0);
    if (userSize) {
      char* userNameBuf = new char[userSize];
      GetEnvironmentVariable("USERNAME", userNameBuf, userSize);
      userName = userNameBuf;
      delete[] userNameBuf;
    } else {
      userName = "Unknown";
    }
    batch_command_ = string("dbsrun dbsbuild -k -p ") +
        userName + string(" -s ");
  } else {
    batch_mode_ = false;
  }
}

BOOL WINAPI SubprocessSet::NotifyInterrupted(DWORD dwCtrlType) {
  if (dwCtrlType == CTRL_C_EVENT || dwCtrlType == CTRL_BREAK_EVENT) {
    if (!PostQueuedCompletionStatus(ioport_, 0, 0, NULL))
      Win32Fatal("PostQueuedCompletionStatus");
    return TRUE;
  }

  return FALSE;
}

Subprocess *SubprocessSet::Add(const string& command) {
  Subprocess *subprocess = new Subprocess;
  if (batch_mode_) {
    procs_to_batch_.push_back(SubProc(subprocess, command));
  } else {
    if (!subprocess->Start(this, command)) {
      delete subprocess;
      return 0;
    }
    if (subprocess->child_)
      running_.push_back(subprocess);
    else
      finished_.push(subprocess);
  }
  return subprocess;
}

bool SubprocessSet::DoWork() {
  if (procs_to_batch_.size()) {
    batch_process_ = new BatchSubprocess(procs_to_batch_);
    string full_command = batch_command_ + batch_process_->GetCommand();
    if (!batch_process_->Start(this, full_command)) {
      delete batch_process_;
      batch_process_ = NULL;
      return true;
    }
    if (batch_process_->child_) {
      running_.push_back(batch_process_);
    }
    procs_to_batch_.clear();
  }
  DWORD bytes_read;
  Subprocess* subproc;
  OVERLAPPED* overlapped;

  if (!GetQueuedCompletionStatus(ioport_, &bytes_read, (PULONG_PTR)&subproc,
                                 &overlapped, INFINITE)) {
    if (GetLastError() != ERROR_BROKEN_PIPE)
      Win32Fatal("GetQueuedCompletionStatus");
  }

  if (!subproc) // A NULL subproc indicates that we were interrupted and is
                // delivered by NotifyInterrupted above.
    return true;

  subproc->OnPipeReady();

  RealDiskInterface rdi;
  if (subproc->Done()) {
    if (subproc == batch_process_) {
      ExitStatus err = batch_process_->Finish();
      set<int> successful_jobs;
      map<int, string> job_output;
      batch_process_->ParseOutput(successful_jobs, job_output);
      const std::vector<Subprocess*>& children = batch_process_->GetChildren();
      for (uint32_t child = 0; child < children.size(); ++child) {
        Subprocess* c = children[child];
        c->use_override_status_ = true;
        if (successful_jobs.count(child) == 1) {
          c->override_status_ = ExitSuccess;
        } else {
          c->override_status_ = err;
        }
        c->buf_ = job_output[child];
        finished_.push(c);
      }

      vector<Subprocess*>::iterator end =
          std::remove(running_.begin(), running_.end(), batch_process_);
      if (running_.end() != end) {
        running_.resize(end - running_.begin());
      }
      delete batch_process_;
      batch_process_ = NULL;
    } else {
      vector<Subprocess*>::iterator end =
        std::remove(running_.begin(), running_.end(), subproc);
      if (running_.end() != end) {
        finished_.push(subproc);
        running_.resize(end - running_.begin());
      }
    }
  }

  return false;
}

Subprocess* SubprocessSet::NextFinished() {
  if (finished_.empty())
    return NULL;
  Subprocess* subproc = finished_.front();
  finished_.pop();
  return subproc;
}

void SubprocessSet::Clear() {
  for (vector<Subprocess*>::iterator i = running_.begin();
       i != running_.end(); ++i) {
    if ((*i)->child_) {
      if (!GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT,
                                    GetProcessId((*i)->child_))) {
        Win32Fatal("GenerateConsoleCtrlEvent");
      }
    }
  }
  for (vector<Subprocess*>::iterator i = running_.begin();
       i != running_.end(); ++i)
    delete *i;
  running_.clear();
}

static string GetTempFileName() {
  char tempPath[MAX_PATH];
  char tempName[MAX_PATH];
  string temp_file;
  DWORD ret = GetTempPath(MAX_PATH, tempPath);
  if (ret > MAX_PATH || ret == 0) {
    return temp_file;
  }
  UINT uret = GetTempFileName(tempPath, "script", 0, tempName);
  if (uret == 0) {
    return temp_file;
  }

  temp_file = tempName;
  return temp_file;
}

BatchSubprocess::BatchSubprocess(const vector<SubProc>& batch_procs) {
  script_filename_= GetTempFileName();
  if (script_filename_.empty()) {
    Win32Fatal("GetTempFileName");
    return;
  }

  // Write all the commands to a batch file and add these
  // subprocs to our internal list.
  string script_contents;
  for (uint32_t i = 0; i < batch_procs.size(); ++i) {
    const SubProc& sp = batch_procs[i];
    char batch_id[128];
    _snprintf(batch_id, sizeof(batch_id), "echo __batchitem__=%d\n", i);
    char batch_complete[128];
    _snprintf(batch_complete, sizeof(batch_complete),
        " && echo __batchitem_complete__=%d\n", i);

    // Each command is like:
    // "echo __batchitem__=5"
    // "cl.exe /c source0.c ... && echo __batchitem_complete__=5"
    // This way we can parse a) the output for each job and
    // b) the exit status for each job (the _complete string only echos
    // on job success.)
    script_contents += batch_id +
        sp.second +
        batch_complete;
    AppendChild(sp.first);
  }

  // Write the batch file.
  RealDiskInterface rdi;
  rdi.WriteFile(script_filename_, script_contents);
}

BatchSubprocess::~BatchSubprocess() {
  RealDiskInterface rdi;
  rdi.RemoveFile(script_filename_);
}

const string& BatchSubprocess::GetCommand() const {
  return script_filename_;
}

void BatchSubprocess::ParseOutput(
    set<int>& successful_jobs, map<int, string>& job_output) {
  // Output is intermixed job output with __batchitem_complete__=XXXX strings.
  // Go through and remove all the __batchitem_complete__ tokens to figure out
  // which jobs succeeded, and get the real output.

  // Remember which pieces of buf_ we want to keep.
  typedef vector<pair<size_t, size_t> > SizeRange;
  SizeRange ranges_to_keep;

  // By default keep the whole string.
  ranges_to_keep.push_back(make_pair(0, buf_.length()));

  const string complete_token = "__batchitem_complete__";

  size_t loc = buf_.find(complete_token);
  size_t start_pos = 0;
  while (loc != string::npos) {
    // Shorten the last element.
    ranges_to_keep.back().second = loc;
    size_t jobIdStart = loc + complete_token.length() + 1;
    int jobId = atoi(&buf_[jobIdStart]);
    successful_jobs.insert(jobId);
    start_pos = buf_.find_first_of('\n', jobIdStart) + 1;
    ranges_to_keep.push_back(make_pair(start_pos, buf_.length()));
    // Get the next one.
    loc = buf_.find(complete_token, start_pos);
  }

  string stripped_buf;
  stripped_buf.reserve(buf_.length());

  // Copy everything from buf_ except for the completion tokens.
  // This is much faster than erasing from buf_.
  for (SizeRange::const_iterator it = ranges_to_keep.begin();
       it != ranges_to_keep.end(); ++it) {
    stripped_buf.append(buf_, it->first, it->second - it->first);
  }
  buf_ = stripped_buf;

  // Now we just have the regular output, with a bunch of start
  // tokens that indicate which job's output it is.
  // Add each job's output to the job_output map,
  // and remove it from the batch job's output.
  const string start_token = "__batchitem__";
  loc = buf_.find(start_token);

  if (loc == string::npos) {
    // Exit without modifying buf_. Something went wrong
    // if no jobs were executed.
    return;
  }

  while (loc != string::npos) {
    size_t jobIdStart = loc + start_token.length() + 1;
    int jobId = atoi(&buf_[jobIdStart]);

    // Advance past this token.
    start_pos = buf_.find_first_of('\n', jobIdStart) + 1;

    // Find the next token, if any.
    size_t next_loc = buf_.find(start_token, start_pos);

    size_t output_len = next_loc - start_pos;
    job_output[jobId] = buf_.substr(start_pos, output_len);
    loc = next_loc;
  }

  // Extracted all the output.
  buf_ = "";
}