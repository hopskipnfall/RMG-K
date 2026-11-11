#!/usr/bin/env bash
#
# Reports PE imports that do not exist on Windows 7 SP1.
#
# ./CheckWin7Imports.sh "./Bin/Release"
#
# Not exhaustive (a denylist of well known Windows 8+ imports), but it catches
# the usual "procedure entry point could not be located" crashes.
# Exits non-zero only when STRICT=1 and something was found.

dir="${1:-.}"

bad_dlls='api-ms-win-core-path|api-ms-win-core-winrt|api-ms-win-core-realtime|shcore|bcryptprimitives'
bad_funcs='GetSystemTimePreciseAsFileTime|SetThreadDescription|GetThreadDescription|WaitOnAddress|WakeByAddressSingle|WakeByAddressAll|PrefetchVirtualMemory|CreateFile2|GetOverlappedResultEx|InitializeSynchronizationBarrier|EnterSynchronizationBarrier|DeleteSynchronizationBarrier|GetDpiForWindow|GetDpiForSystem|GetDpiForMonitor|SetProcessDpiAwareness|SetProcessDpiAwarenessContext|SetThreadDpiAwarenessContext|GetThreadDpiAwarenessContext|AreDpiAwarenessContextsEqual|GetSystemMetricsForDpi|AdjustWindowRectExForDpi|EnableNonClientDpiScaling|PathCchCanonicalize|PathCchCanonicalizeEx|PathCchCombine|PathCchCombineEx|VirtualAlloc2|MapViewOfFile3|MapViewOfFileNuma2|OfferVirtualMemory|ReclaimVirtualMemory|DiscardVirtualMemory|SetProcessMitigationPolicy|GetProcessMitigationPolicy|SetProcessValidCallTargets|GetCurrentPackageId|GetCurrentPackageFullName|GetSystemTimeAdjustmentPrecise|QueryInterruptTime|QueryUnbiasedInterruptTimePrecise|GetFileInformationByHandleEx2|SetThreadInformation|GetThreadInformation|SetProcessInformation|GetProcessInformation|SetFileInformationByName|CompareObjectHandles|RtlGetDeviceFamilyInfoEnum|GetTempPath2[AW]|LoadPackagedLibrary|ProcessPrng|CreateRectRgnIndirect2'

found=0
while IFS= read -r -d '' file
do
    dump="$(objdump.exe -p "$file" 2>/dev/null || objdump -p "$file" 2>/dev/null)"
    hits="$(printf '%s\n' "$dump" | grep -E "DLL Name: ($bad_dlls)" | sed 's/^[[:space:]]*//')"
    hits+="$(printf '%s\n' "$dump" | grep -wE "($bad_funcs)" | sed 's/^[[:space:]]*/  /')"
    if [ -n "$hits" ]
    then
        found=1
        echo "=> $file"
        echo "$hits"
    fi
# api-ms-win-*.dll are the app-local UCRT forwarder stubs, not our code
done < <(find "$dir" \( -iname '*.dll' -o -iname '*.exe' \) ! -iname 'api-ms-win-*' -print0)

if [ "$found" = 0 ]
then
    echo "No known Windows 8+ imports found."
elif [ "$STRICT" = 1 ]
then
    exit 1
fi
exit 0
