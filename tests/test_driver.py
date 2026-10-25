"""Testes sem USB real: transporte limitado e integridade da resposta completa."""
import errno
import importlib.machinery
import threading
import time
import unittest

from dho804_usb import driver as drv


class FakeBackend:
    def __init__(self, data=b'', limit=None, failure=None):
        self.data = data
        self.position = 0
        self.limit = limit
        self.failure = failure
        self.read_sizes = []
        self.writes = []
        self.ioctls = []
        self.closed = []

    def open(self, path):
        return 37

    def close(self, fd):
        self.closed.append(fd)

    def flock(self, fd, operation):
        pass

    def ioctl(self, fd, request, argument=0):
        self.ioctls.append(request)
        return 0

    def read(self, fd, count):
        self.read_sizes.append(count)
        if self.failure:
            raise self.failure
        length = min(count, self.limit) if self.limit else count
        result = self.data[self.position:self.position + length]
        self.position += len(result)
        return result

    def write(self, fd, data):
        self.writes.append(bytes(data))
        return len(data)


def ieee(payload):
    length = str(len(payload)).encode()
    return b'#' + str(len(length)).encode() + length + payload + b'\n'


class DriverTests(unittest.TestCase):
    def test_native_open_error_preserves_public_exception(self):
        with self.assertRaises(drv.UsbTmcError):
            drv.UsbTmcInstrument('/nonexistent-dho804-test/usbtmc0')

    def test_invalid_native_timeout_is_rejected_before_open(self):
        for value in (0, -1, 3600001):
            with self.assertRaises(ValueError):
                drv.UsbTmcInstrument('/nonexistent-dho804-test/usbtmc0', timeout_ms=value)

    def test_loaded_module_is_native_extension(self):
        self.assertIsInstance(drv.__loader__, importlib.machinery.ExtensionFileLoader)
        self.assertTrue(any(drv.__file__.endswith(suffix)
                            for suffix in importlib.machinery.EXTENSION_SUFFIXES))
        self.assertEqual(drv.DEFAULT_CHUNK_SIZE, 4000)

    def test_discovery_preserves_injected_backend_and_identity(self):
        identity = b'RIGOL TECHNOLOGIES,DHO804,SERIAL,00.01.05\n'
        io = FakeBackend(identity)
        def factory(device, timeout_ms):
            return drv.UsbTmcInstrument(device, timeout_ms, backend=io)
        instrument, result = drv.descobrir_dho804(
            candidates=['/dev/usbtmc0'],
            identity_reader=lambda _: ('1ab1', '044d', 'SERIAL'),
            instrument_factory=factory)
        with instrument:
            self.assertEqual(result, identity.decode().strip())
            self.assertEqual(instrument.timeout, 5000)
        self.assertNotIn(drv.USBTMC_IOCTL_CLEAR, io.ioctls)

    def test_queries_are_serialized_across_threads(self):
        class EchoBackend(FakeBackend):
            def write(self, fd, data):
                self.data = bytes(data)
                self.position = 0
                time.sleep(0.001)
                return super().write(fd, data)
        io = EchoBackend()
        results = []
        failures = []
        with drv.UsbTmcInstrument(backend=io) as instrument:
            def worker(number):
                try:
                    for index in range(10):
                        command = f'T{number}:{index}?'
                        result = instrument.query(command)
                        results.append((command + '\n', result))
                except BaseException as error:
                    failures.append(repr(error))
            workers = [threading.Thread(target=worker, args=(n,)) for n in range(4)]
            for thread in workers:
                thread.start()
            for thread in workers:
                thread.join(timeout=5)
            self.assertFalse(any(thread.is_alive() for thread in workers))
        self.assertEqual(failures, [])
        self.assertEqual(len(results), 40)
        self.assertTrue(all(expected == result for expected, result in results))

    def test_full_50mb_block_uses_bounded_usb_reads(self):
        payload = bytes(range(256)) * (50_000_000 // 256) + bytes(range(50_000_000 % 256))
        wire = ieee(payload)
        io = FakeBackend(wire)
        with drv.UsbTmcInstrument(backend=io) as instrument:
            instrument.write(':WAVeform:DATA?')
            result = instrument.read_raw()
        self.assertEqual(result, wire)
        self.assertEqual(io.position, len(wire))
        self.assertEqual(max(io.read_sizes), 4000)
        self.assertEqual(io.writes, [b':WAVeform:DATA?\n'])
        self.assertNotIn(drv.USBTMC_IOCTL_CLEAR, io.ioctls)

    def test_large_read_bytes_preserves_surplus_and_chunk_limit(self):
        wire = bytes(range(256)) * 64
        io = FakeBackend(wire)
        with drv.UsbTmcInstrument(backend=io) as instrument:
            self.assertEqual(instrument.read_bytes(2), wire[:2])
            self.assertEqual(instrument.read_bytes(12_345), wire[2:12_347])
            self.assertEqual(instrument.read_bytes(len(wire)-12_347), wire[12_347:])
        self.assertTrue(all(size == 4000 for size in io.read_sizes))

    def test_fragmented_ieee_header_and_payload(self):
        wire = ieee(b'\x00\n\r#' * 29)
        io = FakeBackend(wire, limit=1)
        with drv.UsbTmcInstrument(backend=io) as instrument:
            # read_raw returns at declared payload end; terminator may be separate.
            self.assertEqual(instrument.read_raw(), wire[:-1])
            self.assertEqual(instrument.read_bytes(1), b'\n')

    def test_truncated_payload_is_failure_without_retry_or_clear(self):
        io = FakeBackend(b'#210abc')
        with drv.UsbTmcInstrument(backend=io) as instrument:
            instrument.write(':WAVeform:DATA?')
            with self.assertRaises(drv.UsbTmcError):
                instrument.read_raw()
        self.assertEqual(io.writes, [b':WAVeform:DATA?\n'])
        self.assertNotIn(drv.USBTMC_IOCTL_CLEAR, io.ioctls)

    def test_timeout_propagates_without_query_retry(self):
        io = FakeBackend(failure=OSError(errno.ETIMEDOUT, 'timeout'))
        with drv.UsbTmcInstrument(backend=io) as instrument:
            with self.assertRaises(drv.UsbTmcTimeoutError):
                instrument.query('*IDN?')
        self.assertEqual(io.writes, [b'*IDN?\n'])
        self.assertNotIn(drv.USBTMC_IOCTL_CLEAR, io.ioctls)

    def test_query_has_one_newline_and_close_is_idempotent(self):
        io = FakeBackend(b'RIGOL TECHNOLOGIES,DHO804,SERIAL,00.01.05\n')
        instrument = drv.UsbTmcInstrument(backend=io)
        self.assertTrue(drv.idn_eh_dho804(instrument.query('*IDN?\n')))
        instrument.close()
        instrument.close()
        self.assertEqual(io.writes, [b'*IDN?\n'])
        self.assertEqual(io.closed, [37])
        self.assertNotIn(drv.USBTMC_IOCTL_CLEAR, io.ioctls)

    def test_bridge_reopens_descriptor_without_clear(self):
        old = FakeBackend()
        new = FakeBackend()
        instrument = drv.UsbTmcInstrument(backend=old)
        def factory(**kwargs):
            return drv.UsbTmcInstrument(backend=new, **kwargs)
        bridge = drv.ScpiUsbBridge(instrument, instrument_factory=factory)
        bridge._reopen_usb()
        self.assertEqual(old.closed, [37])
        self.assertEqual(bridge.instrumento.chunk_size, 4000)
        bridge._forget_instrument()
        self.assertEqual(new.closed, [37])
        self.assertNotIn(drv.USBTMC_IOCTL_CLEAR, old.ioctls + new.ioctls)

    def test_bridge_refuses_lan_listener(self):
        with self.assertRaises(ValueError):
            drv.ScpiUsbBridge(None, host='0.0.0.0', instrument_discovery=lambda: None)

    def test_malformed_header_is_rejected(self):
        with drv.UsbTmcInstrument(backend=FakeBackend(b'#xabc')) as instrument:
            with self.assertRaises(drv.UsbTmcError):
                instrument.read_raw()


if __name__ == '__main__':
    unittest.main()
