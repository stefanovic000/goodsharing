"""Bounded, read-only resource sampling of a subprocess this test owns (Windows)."""
import ctypes
from ctypes import wintypes
import os
import threading
import time


class MemoryCounters(ctypes.Structure):
    _fields_ = [('cb', wintypes.DWORD), ('PageFaultCount', wintypes.DWORD)] + [
        (name, ctypes.c_size_t) for name in (
            'PeakWorkingSetSize', 'WorkingSetSize', 'QuotaPeakPagedPoolUsage',
            'QuotaPagedPoolUsage', 'QuotaPeakNonPagedPoolUsage',
            'QuotaNonPagedPoolUsage', 'PagefileUsage', 'PeakPagefileUsage', 'PrivateUsage')]


class ProcessFootprint:
    """Uses the existing Popen handle; never enumerates or opens other processes."""
    def __init__(self, process, role):
        self.process, self.role = process, role
        self.samples, self.errors = [], []
        self.done = threading.Event()
        self.thread = None
        self.started = time.monotonic()
        self.kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        self.psapi = ctypes.WinDLL('psapi', use_last_error=True)
        self.kernel.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
        self.kernel.GetProcessTimes.restype = wintypes.BOOL
        self.kernel.GetProcessHandleCount.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
        self.kernel.GetProcessHandleCount.restype = wintypes.BOOL
        self.psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(MemoryCounters), wintypes.DWORD]
        self.psapi.GetProcessMemoryInfo.restype = wintypes.BOOL

    def sample(self):
        handle = wintypes.HANDLE(int(self.process._handle))
        times = [wintypes.FILETIME() for _ in range(4)]
        if not self.kernel.GetProcessTimes(handle, *(ctypes.byref(t) for t in times)):
            raise ctypes.WinError(ctypes.get_last_error())
        memory, handles = MemoryCounters(), wintypes.DWORD()
        memory.cb = ctypes.sizeof(memory)
        if not self.psapi.GetProcessMemoryInfo(handle, ctypes.byref(memory), memory.cb):
            raise ctypes.WinError(ctypes.get_last_error())
        if not self.kernel.GetProcessHandleCount(handle, ctypes.byref(handles)):
            raise ctypes.WinError(ctypes.get_last_error())
        cpu = sum((t.dwHighDateTime << 32) | t.dwLowDateTime for t in times[2:]) / 10_000_000
        self.samples.append({'elapsed_s': time.monotonic() - self.started, 'cpu_s': cpu,
                             'private_bytes': memory.PrivateUsage, 'working_set_bytes': memory.WorkingSetSize,
                             'handles': handles.value})

    def start(self):
        self.sample()

        def run():
            while not self.done.wait(.25) and self.process.poll() is None:
                try:
                    if len(self.samples) >= 1500:
                        raise RuntimeError('Resource sampler reached its bounded sample limit')
                    self.sample()
                except Exception as error:
                    # The process can exit between poll and GetProcessMemoryInfo.
                    if self.process.poll() is None:
                        self.errors.append(str(error))
                    break
        self.thread = threading.Thread(target=run, name='owned-process-footprint')
        self.thread.start()
        return self

    def stop(self):
        self.done.set()
        if self.thread:
            self.thread.join(timeout=2)
            if self.thread.is_alive():
                self.errors.append('Resource sampler did not stop')
        return self.report()

    def report(self):
        def window(samples):
            if len(samples) < 2:
                return None
            first, last = samples[0], samples[-1]
            duration = last['elapsed_s'] - first['elapsed_s']
            cores = (last['cpu_s'] - first['cpu_s']) / duration
            return {'duration_s': duration, 'cpu_core_equivalents': cores,
                    'cpu_machine_percent': 100 * cores / (os.cpu_count() or 1),
                    'private_start_bytes': first['private_bytes'], 'private_end_bytes': last['private_bytes'],
                    'private_peak_bytes': max(s['private_bytes'] for s in samples),
                    'working_set_peak_bytes': max(s['working_set_bytes'] for s in samples),
                    'handles_start': first['handles'], 'handles_end': last['handles'],
                    'handles_peak': max(s['handles'] for s in samples)}
        end = self.samples[-1]['elapsed_s'] if self.samples else 0
        return {'role': self.role, 'pid': self.process.pid, 'logical_processors': os.cpu_count(),
                'interval_s': .25, 'errors': self.errors,
                'sampled_lifetime': window(self.samples),
                'middle_window_excluding_first_5s_last_2s': window([s for s in self.samples if 5 <= s['elapsed_s'] <= end - 2]),
                'samples': self.samples}
