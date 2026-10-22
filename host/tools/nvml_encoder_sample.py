#!/usr/bin/env python3
"""Read moving-average NVENC telemetry for one explicitly supplied owned PID."""
import ctypes as C
import json
import signal
import sys
import time


class Session(C.Structure):
    _fields_ = [(name, C.c_uint) for name in ('session_id', 'pid', 'vgpu_instance', 'codec',
                                             'width', 'height', 'average_fps', 'average_latency_us')]


def main():
    if len(sys.argv)!=2 or not sys.argv[1].isdigit() or int(sys.argv[1])<=0:
        raise SystemExit('Usage: nvml_encoder_sample OWNED_SERVER_PID')
    pid = int(sys.argv[1])
    running = True
    def stop(*_):
        nonlocal running
        running = False
    signal.signal(signal.SIGTERM, stop)
    nv = C.CDLL('libnvidia-ml.so.1')
    nv.nvmlInit_v2.argtypes = []; nv.nvmlInit_v2.restype = C.c_int
    nv.nvmlShutdown.argtypes = []; nv.nvmlShutdown.restype = C.c_int
    nv.nvmlDeviceGetHandleByIndex_v2.argtypes = [C.c_uint, C.POINTER(C.c_void_p)]
    nv.nvmlDeviceGetHandleByIndex_v2.restype = C.c_int
    nv.nvmlDeviceGetEncoderSessions.argtypes = [C.c_void_p, C.POINTER(C.c_uint), C.POINTER(Session)]
    nv.nvmlDeviceGetEncoderSessions.restype = C.c_int
    nv.nvmlDeviceGetClockInfo.argtypes = [C.c_void_p, C.c_int, C.POINTER(C.c_uint)]
    nv.nvmlDeviceGetClockInfo.restype = C.c_int
    def check(status):
        if status: raise RuntimeError('NVML status '+str(status))
    check(nv.nvmlInit_v2())
    try:
        device = C.c_void_p(); check(nv.nvmlDeviceGetHandleByIndex_v2(0, C.byref(device)))
        start = time.monotonic()
        while running and time.monotonic()-start<20:
            count = C.c_uint(0)
            status = nv.nvmlDeviceGetEncoderSessions(device, C.byref(count), None)
            if status not in (0, 7): check(status)
            if count.value:
                if count.value>1024: raise RuntimeError('Unreasonable session count')
                entries = (Session*count.value)()
                status = nv.nvmlDeviceGetEncoderSessions(device, C.byref(count), entries)
                if status==7: continue  # Session list changed; retry, never read past the allocation.
                check(status)
                for entry in entries[:count.value]:
                    if entry.pid!=pid: continue
                    row = {name: getattr(entry, name) for name,_ in Session._fields_}
                    row['elapsed_seconds'] = round(time.monotonic()-start, 3)
                    row['scope'] = 'Driver moving averages, not per-frame GPU timestamps'
                    for field, clock in (('video_clock_mhz', 3), ('graphics_clock_mhz', 0)):
                        value = C.c_uint()
                        if nv.nvmlDeviceGetClockInfo(device, clock, C.byref(value))==0:
                            row[field] = value.value
                    print(json.dumps(row), flush=True)
            time.sleep(.2)
    finally:
        nv.nvmlShutdown()


if __name__=='__main__':
    main()
