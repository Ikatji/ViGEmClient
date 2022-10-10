/*
MIT License

Copyright (c) 2017-2019 Nefarius Software Solutions e.U. and Contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/


//
// WinAPI
// 
#include <Windows.h>
#include <SetupAPI.h>
#include <initguid.h>
#include <strsafe.h>

//
// Driver shared
// 
#include "ViGEm/km/BusShared.h"
#include "ViGEm/Client.h"
#include <winioctl.h>

//
// STL
// 
#include <cstdlib>
#include <climits>
#include <thread>
#include <functional>

//
// Internal
// 
#include "Internal.h"


#define DBGPRINT(kwszDebugFormatString, ...) _DBGPRINT(__FUNCTIONW__, __LINE__, kwszDebugFormatString, __VA_ARGS__)

VOID _DBGPRINT(LPCWSTR kwszFunction, INT iLineNumber, LPCWSTR kwszDebugFormatString, ...) \
{
	INT cbFormatString = 0;
	va_list args;
	PWCHAR wszDebugString = NULL;
	size_t st_Offset = 0;

	va_start(args, kwszDebugFormatString);

	cbFormatString = _scwprintf(L"[%s:%d] ", kwszFunction, iLineNumber) * sizeof(WCHAR);
	cbFormatString += _vscwprintf(kwszDebugFormatString, args) * sizeof(WCHAR) + 2;

	/* Depending on the size of the format string, allocate space on the stack or the heap. */
	wszDebugString = (PWCHAR)_malloca(cbFormatString);

	/* Populate the buffer with the contents of the format string. */
	StringCbPrintfW(wszDebugString, cbFormatString, L"[%s:%d] ", kwszFunction, iLineNumber);
	StringCbLengthW(wszDebugString, cbFormatString, &st_Offset);
	StringCbVPrintfW(&wszDebugString[st_Offset / sizeof(WCHAR)], cbFormatString - st_Offset, kwszDebugFormatString, args);

	OutputDebugStringW(wszDebugString);

	_freea(wszDebugString);
	va_end(args);
}

static void util_dump_as_hex(PCSTR Prefix, PVOID Buffer, ULONG BufferLength)
{
	const size_t dumpBufferLength = ((BufferLength * sizeof(CHAR)) * 2) + 1;
	PSTR dumpBuffer = static_cast<PSTR>(malloc(dumpBufferLength));
	if (dumpBuffer)
	{
		RtlZeroMemory(dumpBuffer, dumpBufferLength);

		for (ULONG i = 0; i < BufferLength; i++)
		{
			_snprintf_s(&dumpBuffer[i * 2], dumpBufferLength, _TRUNCATE, "%02X", static_cast<PUCHAR>(Buffer)[i]);
		}

		DBGPRINT(
			L"%s - Buffer length: %04d, buffer content: %s",
			Prefix,
			BufferLength,
			dumpBuffer
		);
		free(dumpBuffer);
	}
}


//
// Initializes a virtual gamepad object.
// 
PVIGEM_TARGET FORCEINLINE VIGEM_TARGET_ALLOC_INIT(
	_In_ VIGEM_TARGET_TYPE Type
)
{
	auto target = static_cast<PVIGEM_TARGET>(malloc(sizeof(VIGEM_TARGET)));

	if (!target)
		return nullptr;

	memset(target, 0, sizeof(VIGEM_TARGET));

	target->Size = sizeof(VIGEM_TARGET);
	target->State = VIGEM_TARGET_INITIALIZED;
	target->Type = Type;
	return target;
}

PVIGEM_CLIENT vigem_alloc()
{
	const auto driver = static_cast<PVIGEM_CLIENT>(malloc(sizeof(VIGEM_CLIENT)));

	if (!driver)
		return nullptr;

	RtlZeroMemory(driver, sizeof(VIGEM_CLIENT));
	driver->hBusDevice = INVALID_HANDLE_VALUE;

	return driver;
}

void vigem_free(PVIGEM_CLIENT vigem)
{
	if (vigem)
		free(vigem);
}

VIGEM_ERROR vigem_connect(PVIGEM_CLIENT vigem)
{
	if (!vigem)
		return VIGEM_ERROR_BUS_INVALID_HANDLE;

	SP_DEVICE_INTERFACE_DATA deviceInterfaceData = { 0 };
	deviceInterfaceData.cbSize = sizeof(deviceInterfaceData);
	DWORD memberIndex = 0;
	DWORD requiredSize = 0;
	auto error = VIGEM_ERROR_BUS_NOT_FOUND;

	// check for already open handle as re-opening accidentally would destroy all live targets
	if (vigem->hBusDevice != INVALID_HANDLE_VALUE)
	{
		return VIGEM_ERROR_BUS_ALREADY_CONNECTED;
	}

	const auto deviceInfoSet = SetupDiGetClassDevs(
		&GUID_DEVINTERFACE_BUSENUM_VIGEM,
		nullptr,
		nullptr,
		DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
	);

	// enumerate device instances
	while (SetupDiEnumDeviceInterfaces(
		deviceInfoSet,
		nullptr,
		&GUID_DEVINTERFACE_BUSENUM_VIGEM,
		memberIndex++,
		&deviceInterfaceData
	))
	{
		// get required target buffer size
		SetupDiGetDeviceInterfaceDetail(deviceInfoSet, &deviceInterfaceData, nullptr, 0, &requiredSize, nullptr);

		// allocate target buffer
		const auto detailDataBuffer = static_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA>(malloc(requiredSize));
		detailDataBuffer->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

		// get detail buffer
		if (!SetupDiGetDeviceInterfaceDetail(
			deviceInfoSet,
			&deviceInterfaceData,
			detailDataBuffer,
			requiredSize,
			&requiredSize,
			nullptr
		))
		{
			free(detailDataBuffer);
			error = VIGEM_ERROR_BUS_NOT_FOUND;
			continue;
		}

		// bus found, open it
		vigem->hBusDevice = CreateFile(
			detailDataBuffer->DevicePath,
			GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE,
			nullptr,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OVERLAPPED,
			nullptr
		);

		// check bus open result
		if (vigem->hBusDevice == INVALID_HANDLE_VALUE)
		{
			error = VIGEM_ERROR_BUS_ACCESS_FAILED;
			free(detailDataBuffer);
			continue;
		}

		DWORD transferred = 0;
		OVERLAPPED lOverlapped = { 0 };
		lOverlapped.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

		VIGEM_CHECK_VERSION version;
		VIGEM_CHECK_VERSION_INIT(&version, VIGEM_COMMON_VERSION);

		// send compiled library version to driver to check compatibility
		DeviceIoControl(
			vigem->hBusDevice,
			IOCTL_VIGEM_CHECK_VERSION,
			&version,
			version.Size,
			nullptr,
			0,
			&transferred,
			&lOverlapped
		);

		// wait for result
		if (GetOverlappedResult(vigem->hBusDevice, &lOverlapped, &transferred, TRUE) != 0)
		{
			error = VIGEM_ERROR_NONE;
			free(detailDataBuffer);
			CloseHandle(lOverlapped.hEvent);
			break;
		}

		error = VIGEM_ERROR_BUS_VERSION_MISMATCH;

		CloseHandle(lOverlapped.hEvent);
		free(detailDataBuffer);
	}

	SetupDiDestroyDeviceInfoList(deviceInfoSet);

	return error;
}

void vigem_disconnect(PVIGEM_CLIENT vigem)
{
	if (!vigem)
		return;

	if (vigem->hBusDevice != INVALID_HANDLE_VALUE)
	{
		CloseHandle(vigem->hBusDevice);

		RtlZeroMemory(vigem, sizeof(VIGEM_CLIENT));
		vigem->hBusDevice = INVALID_HANDLE_VALUE;
	}
}

BOOLEAN vigem_target_is_waitable_add_supported(PVIGEM_TARGET target)
{
	//
	// Safety check to make people use the older functions and not cause issues
	// Should never pass in an invalid target but doesn't hurt to check.
	//
	if (!target)
		return FALSE;

	// TODO: Replace all this with a more robust version check system
	return !target->IsWaitReadyUnsupported;
}

PVIGEM_TARGET vigem_target_x360_alloc(void)
{
	const auto target = VIGEM_TARGET_ALLOC_INIT(Xbox360Wired);

	if (!target)
		return nullptr;

	target->VendorId = 0x045E;
	target->ProductId = 0x028E;

	return target;
}

PVIGEM_TARGET vigem_target_ds4_alloc(void)
{
	const auto target = VIGEM_TARGET_ALLOC_INIT(DualShock4Wired);

	if (!target)
		return nullptr;

	target->VendorId = 0x054C;
	target->ProductId = 0x05C4;

	return target;
}

void vigem_target_free(PVIGEM_TARGET target)
{
	if (target)
		free(target);
}

VIGEM_ERROR vigem_target_add(PVIGEM_CLIENT vigem, PVIGEM_TARGET target)
{
	VIGEM_ERROR error = VIGEM_ERROR_NO_FREE_SLOT;
	DWORD transferred = 0;
	VIGEM_PLUGIN_TARGET plugin;
	VIGEM_WAIT_DEVICE_READY devReady;
	OVERLAPPED olPlugIn = { 0 };
	olPlugIn.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	OVERLAPPED olWait = { 0 };
	olWait.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

	do {
		if (!vigem)
		{
			error = VIGEM_ERROR_BUS_INVALID_HANDLE;
			break;
		}

		if (!target)
		{
			error = VIGEM_ERROR_INVALID_TARGET;
			break;
		}

		if (vigem->hBusDevice == INVALID_HANDLE_VALUE)
		{
			error = VIGEM_ERROR_BUS_NOT_FOUND;
			break;
		}

		if (target->State == VIGEM_TARGET_NEW)
		{
			error = VIGEM_ERROR_TARGET_UNINITIALIZED;
			break;
		}

		if (target->State == VIGEM_TARGET_CONNECTED)
		{
			error = VIGEM_ERROR_ALREADY_CONNECTED;
			break;
		}

		//
		// TODO: this is mad stupid, redesign, so that the bus fills the assigned slot
		// 
		for (target->SerialNo = 1; target->SerialNo <= VIGEM_TARGETS_MAX; target->SerialNo++)
		{
			VIGEM_PLUGIN_TARGET_INIT(&plugin, target->SerialNo, target->Type);

			plugin.VendorId = target->VendorId;
			plugin.ProductId = target->ProductId;

			/*
			 * Request plugin of device. This is an inherently asynchronous operation,
			 * which is addressed differently through the history of the driver design.
			 * Pre-v1.17 this request was kept pending until the child was deemed operational
			 * which unfortunately causes synchronization issues on some systems.
			 * Starting with v1.17 "waiting" for full power-up is done with an additional
			 * IOCTL that is sent immediately after and kept pending until the driver
			 * reports that the device can receive report updates. The following section
			 * and error handling is designed to achieve transparent backwards compatibility
			 * to not break applications using the pre-v1.17 client SDK. This is not a 100%
			 * perfect and can cause other functions to fail if called too soon but
			 * hopefully the applications will just ignore these errors and retry ;)
			 */
			DeviceIoControl(
				vigem->hBusDevice,
				IOCTL_VIGEM_PLUGIN_TARGET,
				&plugin,
				plugin.Size,
				nullptr,
				0,
				&transferred,
				&olPlugIn
			);

			//
			// This should return fairly immediately >=v1.17
			// 
			if (GetOverlappedResult(vigem->hBusDevice, &olPlugIn, &transferred, TRUE) != 0)
			{
				/*
				 * This function is announced to be blocking/synchronous, a concept that
				 * doesn't reflect the way the bus driver/PNP manager bring child devices
				 * to life. Therefore, we send another IOCTL which will be kept pending
				 * until the bus driver has been notified that the child device has
				 * reached a state that is deemed operational. This request is only
				 * supported on drivers v1.17 or higher, so gracefully cause errors
				 * of this call as a potential success and keep the device plugged in.
				 */
				VIGEM_WAIT_DEVICE_READY_INIT(&devReady, plugin.SerialNo);

				DeviceIoControl(
					vigem->hBusDevice,
					IOCTL_VIGEM_WAIT_DEVICE_READY,
					&devReady,
					devReady.Size,
					nullptr,
					0,
					&transferred,
					&olWait
				);

				if (GetOverlappedResult(vigem->hBusDevice, &olWait, &transferred, TRUE) != 0)
				{
					target->State = VIGEM_TARGET_CONNECTED;

					error = VIGEM_ERROR_NONE;
					break;
				}

				//
				// Backwards compatibility with version pre-1.17, where this IOCTL doesn't exist
				// 
				if (GetLastError() == ERROR_INVALID_PARAMETER)
				{
					target->State = VIGEM_TARGET_CONNECTED;
					target->IsWaitReadyUnsupported = true;

					error = VIGEM_ERROR_NONE;
					break;
				}

				//
				// Don't leave device connected if the wait call failed
				// 
				error = vigem_target_remove(vigem, target);
				break;
			}
		}
	} while (false);

	if (olPlugIn.hEvent)
		CloseHandle(olPlugIn.hEvent);

	if (olWait.hEvent)
		CloseHandle(olWait.hEvent);

	return error;
}

VIGEM_ERROR vigem_target_add_async(PVIGEM_CLIENT vigem, PVIGEM_TARGET target, PFN_VIGEM_TARGET_ADD_RESULT result)
{
	if (!vigem)
		return VIGEM_ERROR_BUS_INVALID_HANDLE;

	if (!target)
		return VIGEM_ERROR_INVALID_TARGET;

	if (vigem->hBusDevice == INVALID_HANDLE_VALUE)
		return VIGEM_ERROR_BUS_NOT_FOUND;

	if (target->State == VIGEM_TARGET_NEW)
		return VIGEM_ERROR_TARGET_UNINITIALIZED;

	if (target->State == VIGEM_TARGET_CONNECTED)
		return VIGEM_ERROR_ALREADY_CONNECTED;

	std::thread _async{
		[](
		PVIGEM_TARGET _Target,
		PVIGEM_CLIENT _Client,
		PFN_VIGEM_TARGET_ADD_RESULT _Result)
		{
			const auto error = vigem_target_add(_Client, _Target);

			_Result(_Client, _Target, error);
		},
		target, vigem, result
	};

	_async.detach();

	return VIGEM_ERROR_NONE;
}

VIGEM_ERROR vigem_target_remove(PVIGEM_CLIENT vigem, PVIGEM_TARGET target)
{
	if (!vigem)
		return VIGEM_ERROR_BUS_INVALID_HANDLE;

	if (!target)
		return VIGEM_ERROR_INVALID_TARGET;

	if (vigem->hBusDevice == INVALID_HANDLE_VALUE)
		return VIGEM_ERROR_BUS_NOT_FOUND;

	if (target->State == VIGEM_TARGET_NEW)
		return VIGEM_ERROR_TARGET_UNINITIALIZED;

	if (target->State != VIGEM_TARGET_CONNECTED)
		return VIGEM_ERROR_TARGET_NOT_PLUGGED_IN;

	VIGEM_UNPLUG_TARGET unplug;
	DEVICE_IO_CONTROL_BEGIN;

	VIGEM_UNPLUG_TARGET_INIT(&unplug, target->SerialNo);

	DeviceIoControl(
		vigem->hBusDevice,
		IOCTL_VIGEM_UNPLUG_TARGET,
		&unplug,
		unplug.Size,
		nullptr,
		0,
		&transferred,
		&lOverlapped
	);

	if (GetOverlappedResult(vigem->hBusDevice, &lOverlapped, &transferred, TRUE) != 0)
	{
		target->State = VIGEM_TARGET_DISCONNECTED;
		DEVICE_IO_CONTROL_END;

		return VIGEM_ERROR_NONE;
	}

	DEVICE_IO_CONTROL_END;

	return VIGEM_ERROR_REMOVAL_FAILED;
}

VIGEM_ERROR vigem_target_x360_register_notification(
	PVIGEM_CLIENT vigem,
	PVIGEM_TARGET target,
	PFN_VIGEM_X360_NOTIFICATION notification,
	LPVOID userData
)
{
	if (!vigem)
		return VIGEM_ERROR_BUS_INVALID_HANDLE;

	if (!target)
		return VIGEM_ERROR_INVALID_TARGET;

	if (vigem->hBusDevice == INVALID_HANDLE_VALUE)
		return VIGEM_ERROR_BUS_NOT_FOUND;

	if (target->SerialNo == 0 || notification == nullptr)
		return VIGEM_ERROR_INVALID_TARGET;

	if (target->Notification == reinterpret_cast<FARPROC>(notification))
		return VIGEM_ERROR_CALLBACK_ALREADY_REGISTERED;

	target->Notification = reinterpret_cast<FARPROC>(notification);
	target->NotificationUserData = userData;

	if (target->CancelNotificationThreadEvent == nullptr)
		target->CancelNotificationThreadEvent = CreateEvent(
			nullptr,
			TRUE,
			FALSE,
			nullptr
		);
	else
		ResetEvent(target->CancelNotificationThreadEvent);

	std::thread _async{
		[](
		PVIGEM_TARGET _Target,
		PVIGEM_CLIENT _Client,
		LPVOID _UserData)
		{
			DEVICE_IO_CONTROL_BEGIN;

			XUSB_REQUEST_NOTIFICATION xrn;
			XUSB_REQUEST_NOTIFICATION_INIT(&xrn, _Target->SerialNo);

			do
			{
				DeviceIoControl(
					_Client->hBusDevice,
					IOCTL_XUSB_REQUEST_NOTIFICATION,
					&xrn,
					xrn.Size,
					&xrn,
					xrn.Size,
					&transferred,
					&lOverlapped
				);

				if (GetOverlappedResult(_Client->hBusDevice, &lOverlapped, &transferred, TRUE) != 0)
				{
					if (_Target->Notification == nullptr)
					{
						DEVICE_IO_CONTROL_END;
						return;
					}

					reinterpret_cast<PFN_VIGEM_X360_NOTIFICATION>(_Target->Notification)(
						_Client, _Target, xrn.LargeMotor, xrn.SmallMotor, xrn.LedNumber, _UserData
					);

					continue;
				}

				if (GetLastError() == ERROR_ACCESS_DENIED || GetLastError() == ERROR_OPERATION_ABORTED)
				{
					DEVICE_IO_CONTROL_END;
					return;
				}
			} while (TRUE);
		},
		target, vigem, userData
	};

	_async.detach();

	return VIGEM_ERROR_NONE;
}

VIGEM_ERROR vigem_target_ds4_register_notification(
	PVIGEM_CLIENT vigem,
	PVIGEM_TARGET target,
	PFN_VIGEM_DS4_NOTIFICATION notification,
	LPVOID userData
)
{
	if (!vigem)
		return VIGEM_ERROR_BUS_INVALID_HANDLE;

	if (!target)
		return VIGEM_ERROR_INVALID_TARGET;

	if (vigem->hBusDevice == INVALID_HANDLE_VALUE)
		return VIGEM_ERROR_BUS_NOT_FOUND;

	if (target->SerialNo == 0 || notification == nullptr)
		return VIGEM_ERROR_INVALID_TARGET;

	if (target->Notification == reinterpret_cast<FARPROC>(notification))
		return VIGEM_ERROR_CALLBACK_ALREADY_REGISTERED;

	target->Notification = reinterpret_cast<FARPROC>(notification);
	target->NotificationUserData = userData;

	if (target->CancelNotificationThreadEvent == 0)
		target->CancelNotificationThreadEvent = CreateEvent(
			nullptr,
			TRUE,
			FALSE,
			nullptr
		);
	else
		ResetEvent(target->CancelNotificationThreadEvent);

	std::thread _async{
		[](
		PVIGEM_TARGET _Target,
		PVIGEM_CLIENT _Client,
		LPVOID _UserData)
		{
			DEVICE_IO_CONTROL_BEGIN;

			DS4_REQUEST_NOTIFICATION ds4rn;
			DS4_REQUEST_NOTIFICATION_INIT(&ds4rn, _Target->SerialNo);

			do
			{
				DeviceIoControl(
					_Client->hBusDevice,
					IOCTL_DS4_REQUEST_NOTIFICATION,
					&ds4rn,
					ds4rn.Size,
					&ds4rn,
					ds4rn.Size,
					&transferred,
					&lOverlapped
				);

				if (GetOverlappedResult(_Client->hBusDevice, &lOverlapped, &transferred, TRUE) != 0)
				{
					if (_Target->Notification == nullptr)
					{
						DEVICE_IO_CONTROL_END;
						return;
					}

					reinterpret_cast<PFN_VIGEM_DS4_NOTIFICATION>(_Target->Notification)(
						_Client, _Target, ds4rn.Report.LargeMotor,
						ds4rn.Report.SmallMotor,
						ds4rn.Report.LightbarColor, _UserData
					);

					continue;
				}

				if (GetLastError() == ERROR_ACCESS_DENIED || GetLastError() == ERROR_OPERATION_ABORTED)
				{
					DEVICE_IO_CONTROL_END;
					return;
				}
			} while (TRUE);
		},
		target, vigem, userData
	};

	_async.detach();

	return VIGEM_ERROR_NONE;
}

void vigem_target_x360_unregister_notification(PVIGEM_TARGET target)
{
	if (target->CancelNotificationThreadEvent != 0)
		SetEvent(target->CancelNotificationThreadEvent);

	if (target->CancelNotificationThreadEvent != nullptr)
	{
		CloseHandle(target->CancelNotificationThreadEvent);
		target->CancelNotificationThreadEvent = nullptr;
	}

	target->Notification = nullptr;
	target->NotificationUserData = nullptr;
}

void vigem_target_ds4_unregister_notification(PVIGEM_TARGET target)
{
	vigem_target_x360_unregister_notification(target); // The same x360_unregister handler works for DS4_unregister also
}

void vigem_target_set_vid(PVIGEM_TARGET target, USHORT vid)
{
	target->VendorId = vid;
}

void vigem_target_set_pid(PVIGEM_TARGET target, USHORT pid)
{
	target->ProductId = pid;
}

USHORT vigem_target_get_vid(PVIGEM_TARGET target)
{
	return target->VendorId;
}

USHORT vigem_target_get_pid(PVIGEM_TARGET target)
{
	return target->ProductId;
}

VIGEM_ERROR vigem_target_x360_update(
	PVIGEM_CLIENT vigem,
	PVIGEM_TARGET target,
	XUSB_REPORT report
)
{
	if (!vigem)
		return VIGEM_ERROR_BUS_INVALID_HANDLE;

	if (!target)
		return VIGEM_ERROR_INVALID_TARGET;

	if (vigem->hBusDevice == INVALID_HANDLE_VALUE)
		return VIGEM_ERROR_BUS_NOT_FOUND;

	if (target->SerialNo == 0)
		return VIGEM_ERROR_INVALID_TARGET;

	DEVICE_IO_CONTROL_BEGIN;

	XUSB_SUBMIT_REPORT xsr;
	XUSB_SUBMIT_REPORT_INIT(&xsr, target->SerialNo);

	xsr.Report = report;

	DeviceIoControl(
		vigem->hBusDevice,
		IOCTL_XUSB_SUBMIT_REPORT,
		&xsr,
		xsr.Size,
		nullptr,
		0,
		&transferred,
		&lOverlapped
	);

	if (GetOverlappedResult(vigem->hBusDevice, &lOverlapped, &transferred, TRUE) == 0)
	{
		if (GetLastError() == ERROR_ACCESS_DENIED)
		{
			DEVICE_IO_CONTROL_END;
			return VIGEM_ERROR_INVALID_TARGET;
		}
	}

	DEVICE_IO_CONTROL_END;

	return VIGEM_ERROR_NONE;
}

VIGEM_ERROR vigem_target_ds4_update(
	PVIGEM_CLIENT vigem,
	PVIGEM_TARGET target,
	DS4_REPORT report
)
{
	if (!vigem)
		return VIGEM_ERROR_BUS_INVALID_HANDLE;

	if (!target)
		return VIGEM_ERROR_INVALID_TARGET;

	if (vigem->hBusDevice == INVALID_HANDLE_VALUE)
		return VIGEM_ERROR_BUS_NOT_FOUND;

	if (target->SerialNo == 0)
		return VIGEM_ERROR_INVALID_TARGET;

	DEVICE_IO_CONTROL_BEGIN;

	DS4_SUBMIT_REPORT dsr;
	DS4_SUBMIT_REPORT_INIT(&dsr, target->SerialNo);

	dsr.Report = report;

	DeviceIoControl(
		vigem->hBusDevice,
		IOCTL_DS4_SUBMIT_REPORT,
		&dsr,
		dsr.Size,
		nullptr,
		0,
		&transferred,
		&lOverlapped
	);

	if (GetOverlappedResult(vigem->hBusDevice, &lOverlapped, &transferred, TRUE) == 0)
	{
		if (GetLastError() == ERROR_ACCESS_DENIED)
		{
			DEVICE_IO_CONTROL_END;
			return VIGEM_ERROR_INVALID_TARGET;
		}
	}

	DEVICE_IO_CONTROL_END;

	return VIGEM_ERROR_NONE;
}

VIGEM_ERROR vigem_target_ds4_update_ex(PVIGEM_CLIENT vigem, PVIGEM_TARGET target, DS4_REPORT_EX report)
{
	if (!vigem)
		return VIGEM_ERROR_BUS_INVALID_HANDLE;

	if (!target)
		return VIGEM_ERROR_INVALID_TARGET;

	if (vigem->hBusDevice == INVALID_HANDLE_VALUE)
		return VIGEM_ERROR_BUS_NOT_FOUND;

	if (target->SerialNo == 0)
		return VIGEM_ERROR_INVALID_TARGET;

	DEVICE_IO_CONTROL_BEGIN;

	DS4_SUBMIT_REPORT_EX dsr;
	DS4_SUBMIT_REPORT_EX_INIT(&dsr, target->SerialNo);

	dsr.Report = report;

	DeviceIoControl(
		vigem->hBusDevice,
		IOCTL_DS4_SUBMIT_REPORT, // Same IOCTL, just different size
		&dsr,
		dsr.Size,
		nullptr,
		0,
		&transferred,
		&lOverlapped
	);

	if (GetOverlappedResult(vigem->hBusDevice, &lOverlapped, &transferred, TRUE) == 0)
	{
		if (GetLastError() == ERROR_ACCESS_DENIED)
		{
			DEVICE_IO_CONTROL_END;
			return VIGEM_ERROR_INVALID_TARGET;
		}

		/*
		 * NOTE: this will not happen on v1.16 due to NTSTATUS accidentally been set
		 * as STATUS_SUCCESS when the submitted buffer size wasn't the expected one.
		 * For backwards compatibility this function will silently fail (not cause
		 * report updates) when run with the v1.16 driver. This API was introduced
		 * with v1.17 so it won't affect existing applications built before.
		 */
		if (GetLastError() == ERROR_INVALID_PARAMETER)
		{
			DEVICE_IO_CONTROL_END;
			return VIGEM_ERROR_NOT_SUPPORTED;
		}
	}

	DEVICE_IO_CONTROL_END;

	return VIGEM_ERROR_NONE;
}

ULONG vigem_target_get_index(PVIGEM_TARGET target)
{
	return target->SerialNo;
}

VIGEM_TARGET_TYPE vigem_target_get_type(PVIGEM_TARGET target)
{
	return target->Type;
}

BOOL vigem_target_is_attached(PVIGEM_TARGET target)
{
	return (target->State == VIGEM_TARGET_CONNECTED);
}

VIGEM_ERROR vigem_target_x360_get_user_index(
	PVIGEM_CLIENT vigem,
	PVIGEM_TARGET target,
	PULONG index
)
{
	if (!vigem)
		return VIGEM_ERROR_BUS_INVALID_HANDLE;

	if (!target)
		return VIGEM_ERROR_INVALID_TARGET;

	if (vigem->hBusDevice == INVALID_HANDLE_VALUE)
		return VIGEM_ERROR_BUS_NOT_FOUND;

	if (target->SerialNo == 0 || target->Type != Xbox360Wired)
		return VIGEM_ERROR_INVALID_TARGET;

	if (!index)
		return VIGEM_ERROR_INVALID_PARAMETER;

	DEVICE_IO_CONTROL_BEGIN;

	XUSB_GET_USER_INDEX gui;
	XUSB_GET_USER_INDEX_INIT(&gui, target->SerialNo);

	DeviceIoControl(
		vigem->hBusDevice,
		IOCTL_XUSB_GET_USER_INDEX,
		&gui,
		gui.Size,
		&gui,
		gui.Size,
		&transferred,
		&lOverlapped
	);

	if (GetOverlappedResult(vigem->hBusDevice, &lOverlapped, &transferred, TRUE) == 0)
	{
		const auto error = GetLastError();

		if (error == ERROR_ACCESS_DENIED)
		{
			DEVICE_IO_CONTROL_END;
			return VIGEM_ERROR_INVALID_TARGET;
		}

		if (error == ERROR_INVALID_DEVICE_OBJECT_PARAMETER)
		{
			DEVICE_IO_CONTROL_END;
			return VIGEM_ERROR_XUSB_USERINDEX_OUT_OF_RANGE;
		}
	}

	DEVICE_IO_CONTROL_END;

	*index = gui.UserIndex;

	return VIGEM_ERROR_NONE;
}

VIGEM_ERROR vigem_target_ds4_await_output_report(
	PVIGEM_CLIENT vigem,
	PVIGEM_TARGET target,
	PDS4_OUTPUT_BUFFER buffer
)
{
	if (!vigem)
		return VIGEM_ERROR_BUS_INVALID_HANDLE;

	if (!target)
		return VIGEM_ERROR_INVALID_TARGET;

	if (vigem->hBusDevice == INVALID_HANDLE_VALUE)
		return VIGEM_ERROR_BUS_NOT_FOUND;

	if (target->SerialNo == 0)
		return VIGEM_ERROR_INVALID_TARGET;

	if (!buffer)
		return VIGEM_ERROR_INVALID_PARAMETER;

	DEVICE_IO_CONTROL_BEGIN;

	DS4_AWAIT_OUTPUT await;

retry:
	DS4_AWAIT_OUTPUT_INIT(&await, target->SerialNo);

	DBGPRINT(L"Sending IOCTL_DS4_AWAIT_OUTPUT_AVAILABLE for %d", target->SerialNo);

	DeviceIoControl(
		vigem->hBusDevice,
		IOCTL_DS4_AWAIT_OUTPUT_AVAILABLE,
		&await,
		await.Size,
		&await,
		await.Size,
		&transferred,
		&lOverlapped
	);

	if (GetOverlappedResult(vigem->hBusDevice, &lOverlapped, &transferred, TRUE) == 0)
	{
		const DWORD error = GetLastError();
		DEVICE_IO_CONTROL_END;

		if (error == ERROR_ACCESS_DENIED)
		{
			return VIGEM_ERROR_INVALID_TARGET;
		}

		/*
		 * NOTE: check if the driver has set the same serial number we submitted
		 * to be sure this report belongs to our target device. One queue is used
		 * for all potentially spawned virtual DS4s due to limitations on how
		 * DMF_NotifyUserWithRequestMultiple works in combination with device
		 * objects. The module keeps track on requests issued via the FDO (bus
		 * driver device) but must notify for one to many virtual DS4 PDOs.
		 * Therefore, it may happen that a packet bubbles up that doesn't belong
		 * to our device of interest. The workaround is to check if the serial
		 * remained the same and if not, fetch the next packet until the queue
		 * has been processed in its entirety.
		 */
		if (error == ERROR_SUCCESS && await.SerialNo != target->SerialNo)
		{
			DBGPRINT(L"Serial mismatch, sent %d, got %d", target->SerialNo, await.SerialNo);
			goto retry;
		}

		return VIGEM_ERROR_WINAPI;
	}

	DBGPRINT(L"Dumping buffer for %d", target->SerialNo);
	util_dump_as_hex("vigem_target_ds4_await_output_report", await.Report.Buffer, sizeof(DS4_OUTPUT_BUFFER));

	RtlCopyMemory(buffer, await.Report.Buffer, sizeof(DS4_OUTPUT_BUFFER));

	DEVICE_IO_CONTROL_END;

	return VIGEM_ERROR_NONE;
}

VIGEM_ERROR vigem_target_ds4_await_output_report_timeout(
	PVIGEM_CLIENT vigem,
	PVIGEM_TARGET target,
	DWORD milliseconds,
	PDS4_OUTPUT_BUFFER buffer
)
{
	if (!vigem)
		return VIGEM_ERROR_BUS_INVALID_HANDLE;

	if (!target)
		return VIGEM_ERROR_INVALID_TARGET;

	if (vigem->hBusDevice == INVALID_HANDLE_VALUE)
		return VIGEM_ERROR_BUS_NOT_FOUND;

	if (target->SerialNo == 0)
		return VIGEM_ERROR_INVALID_TARGET;

	if (!buffer)
		return VIGEM_ERROR_INVALID_PARAMETER;

	DEVICE_IO_CONTROL_BEGIN;

	DS4_AWAIT_OUTPUT await;

retry:
	DS4_AWAIT_OUTPUT_INIT(&await, target->SerialNo);

	DBGPRINT(L"Sending IOCTL_DS4_AWAIT_OUTPUT_AVAILABLE for %d", target->SerialNo);

	DeviceIoControl(
		vigem->hBusDevice,
		IOCTL_DS4_AWAIT_OUTPUT_AVAILABLE,
		&await,
		await.Size,
		&await,
		await.Size,
		&transferred,
		&lOverlapped
	);

	if (GetOverlappedResultEx(vigem->hBusDevice, &lOverlapped, &transferred, milliseconds, FALSE) == 0)
	{
		const DWORD error = GetLastError();
		DEVICE_IO_CONTROL_END;

		switch (error)
		{
		case ERROR_ACCESS_DENIED:
			return VIGEM_ERROR_INVALID_TARGET;
		case ERROR_IO_INCOMPLETE:
		case WAIT_TIMEOUT:
			CancelIoEx(vigem->hBusDevice, &lOverlapped);
			return VIGEM_ERROR_TIMED_OUT;
		default:
			break;
		}

		/*
		 * NOTE: check if the driver has set the same serial number we submitted
		 * to be sure this report belongs to our target device. One queue is used
		 * for all potentially spawned virtual DS4s due to limitations on how
		 * DMF_NotifyUserWithRequestMultiple works in combination with device
		 * objects. The module keeps track on requests issued via the FDO (bus
		 * driver device) but must notify for one to many virtual DS4 PDOs.
		 * Therefore, it may happen that a packet bubbles up that doesn't belong
		 * to our device of interest. The workaround is to check if the serial
		 * remained the same and if not, fetch the next packet until the queue
		 * has been processed in its entirety.
		 */
		if (error == ERROR_SUCCESS && await.SerialNo != target->SerialNo)
		{
			DBGPRINT(L"Serial mismatch, sent %d, got %d", target->SerialNo, await.SerialNo);
			goto retry;
		}

		return VIGEM_ERROR_WINAPI;
	}

	DBGPRINT(L"Dumping buffer for %d", target->SerialNo);
	util_dump_as_hex("vigem_target_ds4_await_output_report_timeout", await.Report.Buffer, sizeof(DS4_OUTPUT_BUFFER));

	RtlCopyMemory(buffer, await.Report.Buffer, sizeof(DS4_OUTPUT_BUFFER));

	DEVICE_IO_CONTROL_END;

	return VIGEM_ERROR_NONE;
}
