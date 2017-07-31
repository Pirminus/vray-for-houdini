//
// Copyright (c) 2015-2017, Chaos Software Ltd
//
// V-Ray For Houdini Python IPR Module
//
// ACCESSIBLE SOURCE CODE WITHOUT DISTRIBUTION OF MODIFICATION LICENSE
//
// Full license text: https://github.com/ChaosGroup/vray-for-houdini/blob/master/LICENSE
//

#include "vfh_process_check.h"

#include <Windows.h>
#include <TlHelp32.h>

class Win32ProcessCheck:
	public ProcessCheck
{
public:
	Win32ProcessCheck(OnStop cb, const std::string &name)
		: ProcessCheck(cb, name)
		, pid(0)
		, waitHandle(nullptr)
	{}

	bool start() override;

	bool stop() override;


	DWORD getPID() const;
private:
	DWORD pid;
	HANDLE waitHandle;
};


ProcessCheckPtr makeProcessChecker(ProcessCheck::OnStop cb, const std::string &name) {
	return std::make_unique<Win32ProcessCheck>(cb, name);
}

namespace {
std::shared_ptr<void> shared_handle(void *ptr) {
	return std::shared_ptr<void>(ptr, [](void *handle) {
		if (handle) {
			CloseHandle(handle);
		}
	});
}
}

bool Win32ProcessCheck::start() {
	pid = getPID();
	if (!pid) {
		return false;
	}

	auto process = shared_handle(OpenProcess(SYNCHRONIZE, FALSE, pid));

	if (!process) {
		return false;
	}

	auto callback = [](PVOID context, BOOLEAN) {
		auto *self = reinterpret_cast<Win32ProcessCheck*>(context);
		self->stop();
		self->stopCallback();
	};

	if (!RegisterWaitForSingleObject(&waitHandle, process.get(), callback, this, INFINITE, WT_EXECUTEONLYONCE)) {
		return false;
	}

	return true;
}

bool Win32ProcessCheck::stop() {
	if (waitHandle) {
		UnregisterWait(waitHandle);
		waitHandle = nullptr;
		return true;
	}
	return false;
}

DWORD Win32ProcessCheck::getPID() const {
	auto snapshot = shared_handle(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
	if (!snapshot) {
		return 0;
	}

	PROCESSENTRY32 iterator;
	iterator.dwSize = sizeof(PROCESSENTRY32);
	if (Process32First(snapshot.get(), &iterator)) {
		do {
			if (!strcmp(iterator.szExeFile, processName.c_str()) && iterator.th32ParentProcessID == GetCurrentProcessId()) {
				return iterator.th32ProcessID;
			}
		} while (Process32Next(snapshot.get(), &iterator));
	}

	return 0;
}
