# cython: language_level=3, annotation_typing=False, binding=True
# Fonte autoritativa; o C++ em build/cython e gerado, nunca editado.
#!/usr/bin/env python3
"""Ponte SCPI local para o RIGOL DHO804 conectado por USBTMC.

O manual do DHO800 chama a interface traseira de USB-TMC; LXI e' a interface
de rede. Este driver mantem o meio fisico no USB e publica o mesmo protocolo
SCPI de socket cru usado pelo servidor LXI do aparelho, somente em loopback:

    aplicativo -> TCPIP0::127.0.0.1::5556::SOCKET -> /dev/usbtmcN -> DHO804

Nada e' escutado na Ethernet. O descritor e' aberto com exclusao mutua para
impedir duas sessoes de intercalarem comandos e respostas.
"""

from __future__ import annotations

import argparse
import errno
import fcntl
import logging
import os
from pathlib import Path
import re
import select
import signal
import socket
import struct
import threading
import time
from typing import Optional


LOG = logging.getLogger("dho804-usb-lxi")

# Compatibilidade da classe de baixo nivel. O CLI e o servico nao usam este
# valor: sem --device, passam pela autodeteccao antes de construir a classe.
DEFAULT_DEVICE = "/dev/usbtmc0"
DEFAULT_DEVICE_DIR = "/dev"
DEFAULT_DEVICE_GLOB = "/dev/usbtmc*"
DEFAULT_SYS_CLASS_DIR = "/sys/class/usbmisc"
DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 5556
DEFAULT_TIMEOUT_MS = 5000
DEFAULT_DISCOVERY_TIMEOUT_MS = 1000
# Validado no DHO804: RAM integral com cada leitura USB abaixo de 4096 bytes.
DEFAULT_CHUNK_SIZE = 4000
CLIENT_POLL_SECONDS = 0.5
CLIENT_SEND_STALL_SECONDS = 5.0
CLIENT_SEND_CHUNK_SIZE = 64 * 1024
DHO804_USB_VENDOR_ID = "1ab1"
DHO804_USB_PRODUCT_ID = "044d"

_USBTMC_NODE_RE = re.compile(r"^usbtmc([0-9]+)$")
_TIMEOUT_ERRNOS = {
    codigo for codigo in (
        getattr(errno, "ETIME", None),
        getattr(errno, "ETIMEDOUT", None),
    ) if codigo is not None
}

# include/uapi/linux/usb/tmc.h. As macros abaixo reproduzem _IO/_IOW para os
# ioctls que nao sao publicados pelo modulo fcntl do Python.
_IOC_NRBITS = 8
_IOC_TYPEBITS = 8
_IOC_SIZEBITS = 14
_IOC_NRSHIFT = 0
_IOC_TYPESHIFT = _IOC_NRSHIFT + _IOC_NRBITS
_IOC_SIZESHIFT = _IOC_TYPESHIFT + _IOC_TYPEBITS
_IOC_DIRSHIFT = _IOC_SIZESHIFT + _IOC_SIZEBITS
_IOC_NONE = 0
_IOC_WRITE = 1
USBTMC_IOC_NR = 91


def _ioc(direction: int, kind: int, number: int, size: int) -> int:
    return ((direction << _IOC_DIRSHIFT)
            | (kind << _IOC_TYPESHIFT)
            | (number << _IOC_NRSHIFT)
            | (size << _IOC_SIZESHIFT))


def _io(kind: int, number: int) -> int:
    return _ioc(_IOC_NONE, kind, number, 0)


def _iow(kind: int, number: int, size: int) -> int:
    return _ioc(_IOC_WRITE, kind, number, size)


USBTMC_IOCTL_CLEAR = _io(USBTMC_IOC_NR, 2)
USBTMC_IOCTL_SET_TIMEOUT = _iow(USBTMC_IOC_NR, 10, 4)
USBTMC_IOCTL_EOM_ENABLE = _iow(USBTMC_IOC_NR, 11, 1)
USBTMC_IOCTL_AUTO_ABORT = _iow(USBTMC_IOC_NR, 25, 1)
USBTMC_IOCTL_CANCEL_IO = _io(USBTMC_IOC_NR, 35)
USBTMC_IOCTL_CLEANUP_IO = _io(USBTMC_IOC_NR, 36)


def notify_systemd(mensagem: str) -> bool:
    """Envia sd_notify sem depender de libsystemd; False fora de uma unit."""
    endereco = os.environ.get("NOTIFY_SOCKET")
    if not endereco:
        return False
    if endereco.startswith("@"):
        endereco = "\0" + endereco[1:]
    with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as notificacao:
        notificacao.sendto(mensagem.encode("utf-8"), endereco)
    return True


class UsbTmcError(RuntimeError):
    """Falha do transporte USBTMC."""


class UsbTmcBusyError(UsbTmcError):
    """Outro processo ja' e' dono do transporte."""


class UsbTmcPermissionError(UsbTmcError):
    """O processo nao tem permissao para abrir o no USBTMC."""


class UsbTmcTimeoutError(UsbTmcError):
    """O transporte USBTMC excedeu o prazo configurado."""


class UsbTmcDiscoveryError(UsbTmcError):
    """A autodeteccao nao encontrou um unico DHO804 utilizavel."""


class UsbTmcAmbiguousError(UsbTmcDiscoveryError):
    """Mais de um DHO804 correspondeu a busca automatica."""


def listar_dispositivos_usbtmc(
    device_dir: str = DEFAULT_DEVICE_DIR,
    *,
    listdir=os.listdir,
) -> list[str]:
    """Lista somente ``usbtmcN`` em ordem numerica e sem duplicatas."""
    try:
        nomes = listdir(device_dir)
    except OSError as exc:
        raise UsbTmcDiscoveryError(
            f"nao foi possivel listar {device_dir}: {exc}"
        ) from exc

    encontrados = set()
    for nome in nomes:
        nome = os.fsdecode(nome)
        casamento = _USBTMC_NODE_RE.fullmatch(nome)
        if casamento:
            encontrados.add((int(casamento.group(1)), nome))
    return [
        os.path.join(os.fspath(device_dir), nome)
        for _, nome in sorted(encontrados)
    ]


def ler_identidade_usb_usbtmc(
    device: str,
    *,
    sys_class_dir=DEFAULT_SYS_CLASS_DIR,
) -> tuple[str, str, str]:
    """Le VID, PID e serial no pai USB da interface ``usbtmcN``."""
    nome = os.path.basename(os.fspath(device))
    if _USBTMC_NODE_RE.fullmatch(nome) is None:
        raise UsbTmcDiscoveryError(f"no USBTMC invalido: {device}")
    try:
        interface = (Path(sys_class_dir) / nome / "device").resolve(strict=True)
        dispositivo_usb = interface.parent
        vendor = (dispositivo_usb / "idVendor").read_text(
            encoding="ascii"
        ).strip().lower()
        produto = (dispositivo_usb / "idProduct").read_text(
            encoding="ascii"
        ).strip().lower()
        serial_path = dispositivo_usb / "serial"
        serial = (
            serial_path.read_text(encoding="ascii").strip()
            if serial_path.is_file() else ""
        )
    except (OSError, UnicodeError) as exc:
        raise UsbTmcDiscoveryError(
            f"nao foi possivel identificar {device} pelo sysfs: {exc}"
        ) from exc
    return vendor, produto, serial


def idn_eh_dho804(resposta: str) -> bool:
    """Confere os quatro campos de ``*IDN?`` documentados para o DHO804."""
    campos = [campo.strip() for campo in str(resposta).strip().split(",")]
    return (
        len(campos) == 4
        and all(campos)
        and campos[0].upper() == "RIGOL TECHNOLOGIES"
        and campos[1].upper() == "DHO804"
    )


from libcpp.string cimport string
from libcpp cimport bool as cppbool
from libc.stdint cimport uint32_t

cdef extern from "transport.hpp" namespace "dho804":
    cdef cppclass NativeTransport "dho804::Transport":
        NativeTransport(const string&, uint32_t, size_t) except + nogil
        void set_timeout(uint32_t) except + nogil
        uint32_t timeout() except + nogil
        void set_chunk(size_t) except + nogil
        cppbool closed() except + nogil
        size_t write(const string&) except + nogil
        string read_raw() except + nogil
        string read_bytes(size_t) except + nogil
        string query(const string&) except + nogil
        void clear() except + nogil
        void close() noexcept nogil
    void native_serve "dho804::serve_client"(int, NativeTransport&, cppbool (*)(void*) noexcept, void*) except + nogil

cdef cppbool _native_stop(void* context) noexcept with gil:
    try:
        return (<object>context).is_set()
    except BaseException:
        return True


def _raise_native(error):
    message = str(error)
    if message.startswith("SOCKET:"):
        raise ConnectionError(message) from error
    kind = (UsbTmcTimeoutError if message.startswith("TIMEOUT:") else
            UsbTmcBusyError if message.startswith("BUSY:") else
            UsbTmcPermissionError if message.startswith("PERMISSION:") else UsbTmcError)
    raise kind(message) from error


cdef class _NativeTransport:
    cdef NativeTransport* ptr

    def __cinit__(self, device, timeout_ms, chunk_size):
        cdef string path = os.fsencode(device)
        cdef uint32_t timeout = timeout_ms
        cdef size_t chunk = chunk_size
        try:
            with nogil:
                self.ptr = new NativeTransport(path, timeout, chunk)
        except RuntimeError as error:
            _raise_native(error)

    def __dealloc__(self):
        del self.ptr

    def set_timeout(self, value):
        cdef uint32_t timeout = value
        try:
            with nogil:
                self.ptr.set_timeout(timeout)
        except RuntimeError as error:
            _raise_native(error)

    def set_chunk(self, value):
        cdef size_t chunk = value
        with nogil:
            self.ptr.set_chunk(chunk)

    def write(self, data):
        cdef string wire = data
        cdef size_t count
        try:
            with nogil:
                count = self.ptr.write(wire)
            return count
        except RuntimeError as error:
            _raise_native(error)

    def read_raw(self):
        cdef string data
        try:
            with nogil:
                data = self.ptr.read_raw()
            return <bytes>data
        except RuntimeError as error:
            _raise_native(error)

    def read_bytes(self, value):
        cdef size_t count = value
        cdef string data
        try:
            with nogil:
                data = self.ptr.read_bytes(count)
            return <bytes>data
        except RuntimeError as error:
            _raise_native(error)

    def query(self, data):
        cdef string command = data
        cdef string result
        try:
            with nogil:
                result = self.ptr.query(command)
            return <bytes>result
        except RuntimeError as error:
            _raise_native(error)

    def clear(self):
        try:
            with nogil:
                self.ptr.clear()
        except RuntimeError as error:
            _raise_native(error)

    def close(self):
        with nogil:
            self.ptr.close()

    def serve(self, client, event):
        cdef int fd = client.fileno()
        # client/event/self remain owned by this stack throughout nogil.
        try:
            with nogil:
                native_serve(fd, self.ptr[0], _native_stop, <void*>event)
        except RuntimeError as error:
            _raise_native(error)


class UsbTmcInstrument:
    """Recurso com a parte da API PyVISA usada pelo app SCPI-Codex."""

    def __init__(
        self,
        device: str = DEFAULT_DEVICE,
        timeout_ms: int = DEFAULT_TIMEOUT_MS,
        chunk_size: int = DEFAULT_CHUNK_SIZE,
        backend=None,
    ) -> None:
        self.device = device
        self.chunk_size = int(chunk_size)
        if self.chunk_size < 1:
            raise ValueError("chunk_size precisa ser positivo")
        self.read_termination: Optional[str] = "\n"
        self.write_termination: Optional[str] = "\n"
        self._native = None
        self._backend = backend
        self._fd: Optional[int] = None
        self._timeout_ms = DEFAULT_TIMEOUT_MS
        self._rx = bytearray()
        self._lock = threading.RLock()
        timeout_ms = int(timeout_ms)
        if timeout_ms < 1 or timeout_ms > 3_600_000:
            raise ValueError("timeout deve estar entre 1 e 3600000 ms")
        if backend is None:
            self._native = _NativeTransport(device, timeout_ms, self.chunk_size)
            self._timeout_ms = timeout_ms
        else:
            self._open(timeout_ms)

    def _open(self, timeout_ms: int) -> None:
        try:
            fd = self._backend.open(self.device)
        except OSError as exc:
            if exc.errno in (errno.EBUSY, errno.EAGAIN):
                raise UsbTmcBusyError(
                    f"{self.device} esta ocupado por outro driver"
                ) from exc
            if exc.errno in (errno.EACCES, errno.EPERM):
                raise UsbTmcPermissionError(
                    f"sem permissao para abrir {self.device}: {exc}"
                ) from exc
            raise UsbTmcError(
                f"nao foi possivel abrir {self.device}: {exc}"
            ) from exc
        try:
            self._backend.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except (BlockingIOError, OSError) as exc:
            self._backend.close(fd)
            if isinstance(exc, BlockingIOError) or exc.errno in (
                    errno.EACCES, errno.EAGAIN, errno.EBUSY):
                raise UsbTmcBusyError(
                    f"{self.device} esta ocupado por outro driver"
                ) from exc
            if exc.errno == errno.EPERM:
                raise UsbTmcPermissionError(
                    f"sem permissao para reservar {self.device}: {exc}"
                ) from exc
            raise UsbTmcError(
                f"nao foi possivel reservar {self.device}: {exc}"
            ) from exc

        self._fd = fd
        try:
            self.timeout = timeout_ms
            self._ioctl_opcional(
                USBTMC_IOCTL_EOM_ENABLE, struct.pack("B", 1), "EOM"
            )
            self._ioctl_opcional(
                USBTMC_IOCTL_AUTO_ABORT, struct.pack("B", 1), "auto-abort"
            )
        except Exception:
            self.close()
            raise

    @property
    def timeout(self) -> int:
        return self._timeout_ms

    @timeout.setter
    def timeout(self, valor: int) -> None:
        valor = int(valor)
        if valor < 1 or valor > 3_600_000:
            raise ValueError("timeout deve estar entre 1 e 3600000 ms")
        if self._native is not None:
            self._native.set_timeout(valor)
        elif self._fd is not None:
            try:
                self._backend.ioctl(
                    self._fd,
                    USBTMC_IOCTL_SET_TIMEOUT,
                    struct.pack("I", valor),
                )
            except OSError as exc:
                raise UsbTmcError(
                    f"nao foi possivel aplicar timeout em {self.device}: {exc}"
                ) from exc
        self._timeout_ms = valor

    @property
    def closed(self) -> bool:
        return self._native is None and self._fd is None

    def _ioctl_opcional(self, pedido: int, argumento, nome: str) -> None:
        if self._fd is None:
            return
        try:
            self._backend.ioctl(self._fd, pedido, argumento)
        except OSError as exc:
            LOG.debug("ioctl USBTMC %s indisponivel: %s", nome, exc)

    def _fd_aberto(self) -> int:
        if self._fd is None:
            raise UsbTmcError("transporte USBTMC fechado")
        return self._fd

    def _write_raw_unlocked(self, dados: bytes) -> int:
        if self._native is not None:
            return self._native.write(dados)
        fd = self._fd_aberto()
        enviados = 0
        try:
            while enviados < len(dados):
                n = self._backend.write(fd, dados[enviados:])
                if n <= 0:
                    raise UsbTmcError("o USBTMC aceitou zero bytes")
                enviados += n
        except OSError as exc:
            erro = (
                UsbTmcTimeoutError
                if exc.errno in _TIMEOUT_ERRNOS else UsbTmcError
            )
            prefixo = "tempo esgotado" if erro is UsbTmcTimeoutError else "falha"
            raise erro(
                f"{prefixo} ao escrever em {self.device}: {exc}"
            ) from exc
        return enviados

    def write_raw(self, dados: bytes) -> int:
        with self._lock:
            return self._write_raw_unlocked(bytes(dados))

    def write(self, comando: str) -> int:
        dados = comando.encode("ascii")
        if self.write_termination:
            term = self.write_termination.encode("ascii")
            if not dados.endswith(term):
                dados += term
        return self.write_raw(dados)

    def _read_kernel_unlocked(self, quantidade: int) -> bytes:
        fd = self._fd_aberto()
        try:
            dados = self._backend.read(fd, quantidade)
        except OSError as exc:
            erro = (
                UsbTmcTimeoutError
                if exc.errno in _TIMEOUT_ERRNOS else UsbTmcError
            )
            prefixo = "tempo esgotado" if erro is UsbTmcTimeoutError else "falha"
            raise erro(f"{prefixo} ao ler {self.device}: {exc}") from exc
        if not dados:
            raise UsbTmcError("o DHO804 encerrou a resposta USBTMC sem dados")
        return bytes(dados)

    @staticmethod
    def _tamanho_bloco_ieee(dados: bytes) -> Optional[int]:
        if not dados.startswith(b"#"):
            return None
        if len(dados) < 2:
            return -1
        digito = dados[1] - ord("0")
        if digito < 0 or digito > 9:
            raise UsbTmcError(f"cabecalho IEEE 488.2 invalido: {dados[:16]!r}")
        if digito == 0:
            return None
        if len(dados) < 2 + digito:
            return -1
        campo = dados[2:2 + digito]
        if not campo.isdigit():
            raise UsbTmcError(f"tamanho IEEE 488.2 invalido: {campo!r}")
        return 2 + digito + int(campo)

    def _read_raw_unlocked(self) -> bytes:
        if self._native is not None:
            self._native.set_chunk(self.chunk_size)
            return self._native.read_raw()
        if self._rx:
            dados = bytes(self._rx)
            self._rx.clear()
            return dados

        dados = bytearray(self._read_kernel_unlocked(self.chunk_size))
        esperado = self._tamanho_bloco_ieee(dados)
        while esperado == -1 or (esperado is not None and len(dados) < esperado):
            dados.extend(self._read_kernel_unlocked(self.chunk_size))
            esperado = self._tamanho_bloco_ieee(dados)
        return bytes(dados)

    def read_raw(self) -> bytes:
        with self._lock:
            return self._read_raw_unlocked()

    def read_bytes(self, quantidade: int) -> bytes:
        quantidade = int(quantidade)
        if quantidade < 0:
            raise ValueError("quantidade nao pode ser negativa")
        if quantidade == 0:
            return b""
        with self._lock:
            if self._native is not None:
                self._native.set_chunk(self.chunk_size)
                return self._native.read_bytes(quantidade)
            while len(self._rx) < quantidade:
                self._rx.extend(
                    self._read_kernel_unlocked(self.chunk_size)
                )
            saida = bytes(self._rx[:quantidade])
            del self._rx[:quantidade]
            return saida

    def read(self) -> str:
        return self.read_raw().decode("ascii", "replace")

    def query(self, comando: str) -> str:
        with self._lock:
            dados = comando.encode("ascii")
            if self.write_termination:
                term = self.write_termination.encode("ascii")
                if not dados.endswith(term):
                    dados += term
            if self._native is not None:
                self._native.set_chunk(self.chunk_size)
                return self._native.query(dados).decode("ascii", "replace")
            self._write_raw_unlocked(dados)
            return self._read_raw_unlocked().decode("ascii", "replace")

    def clear(self) -> None:
        with self._lock:
            if self._native is not None:
                return self._native.clear()
            self._rx.clear()
            fd = self._fd_aberto()
            try:
                self._backend.ioctl(fd, USBTMC_IOCTL_CLEAR, 0)
            except OSError as exc:
                raise UsbTmcError(
                    f"falha ao limpar a sessao USBTMC: {exc}"
                ) from exc

    def close(self) -> None:
        with self._lock:
            if self._native is not None:
                self._native.close()
                self._native = None
                return
            if self._fd is None:
                return
            fd, self._fd = self._fd, None
            self._rx.clear()
            try:
                self._backend.ioctl(fd, USBTMC_IOCTL_CLEANUP_IO, 0)
            except OSError:
                pass
            try:
                self._backend.flock(fd, fcntl.LOCK_UN)
            except OSError:
                pass
            self._backend.close(fd)

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()


def descobrir_dho804(
    device: Optional[str] = None,
    timeout_ms: int = DEFAULT_TIMEOUT_MS,
    discovery_timeout_ms: int = DEFAULT_DISCOVERY_TIMEOUT_MS,
    *,
    candidates=None,
    candidate_provider=None,
    identity_reader=None,
    instrument_factory=None,
) -> tuple[UsbTmcInstrument, str]:
    """Abre e devolve o unico DHO804 local, junto com seu ``*IDN?``.

    Sem ``device``, enumera ``/dev/usbtmcN``, filtra o VID:PID pelo sysfs e
    confirma fabricante/modelo por SCPI. Com ``device``, nao enumera nem le o
    sysfs, mas valida o alvo por ``*IDN?`` antes de entrega-lo ao servidor.
    """
    timeout_ms = int(timeout_ms)
    discovery_timeout_ms = int(discovery_timeout_ms)
    explicito = device is not None
    fabrica = instrument_factory or UsbTmcInstrument
    ler_identidade = identity_reader or ler_identidade_usb_usbtmc

    if explicito:
        caminhos = [os.fspath(device)]
    elif candidates is not None:
        caminhos = [os.fspath(item) for item in candidates]
    else:
        provedor = candidate_provider or listar_dispositivos_usbtmc
        caminhos = [os.fspath(item) for item in provedor()]

    if not caminhos:
        raise UsbTmcDiscoveryError(
            f"nenhum dispositivo encontrado em {DEFAULT_DEVICE_GLOB}"
        )

    correspondencias = []
    falhas = []
    ignorados = []
    seriais = {}
    entregue = None
    try:
        for caminho in caminhos:
            if not explicito:
                try:
                    vendor, produto, serial = ler_identidade(caminho)
                except (OSError, UsbTmcError) as exc:
                    falhas.append(f"{caminho}: {exc}")
                    continue
                if (vendor.lower(), produto.lower()) != (
                        DHO804_USB_VENDOR_ID, DHO804_USB_PRODUCT_ID):
                    ignorados.append(
                        f"{caminho}: USB {vendor}:{produto}, nao "
                        f"{DHO804_USB_VENDOR_ID}:{DHO804_USB_PRODUCT_ID}"
                    )
                    continue
                seriais[caminho] = serial

            prazo = timeout_ms if explicito else discovery_timeout_ms
            try:
                instrumento = fabrica(caminho, prazo)
            except UsbTmcError as exc:
                falhas.append(f"{caminho}: {exc}")
                continue

            try:
                idn = instrumento.query("*IDN?").strip()
                if idn_eh_dho804(idn):
                    correspondencias.append((instrumento, idn))
                    # A lista assume a posse ate sabermos se a escolha e unica.
                    instrumento = None
                else:
                    ignorados.append(f"{caminho}: *IDN?={idn!r}")
            except UsbTmcError as exc:
                falhas.append(f"{caminho}: {exc}")
            finally:
                if instrumento is not None:
                    instrumento.close()

        if len(correspondencias) > 1:
            caminhos_ambiguos = [
                item.device for item, _ in correspondencias
            ]
            raise UsbTmcAmbiguousError(
                "mais de um DHO804 encontrado: "
                + ", ".join(caminhos_ambiguos)
                + "; use --device CAMINHO"
            )

        if len(correspondencias) == 1:
            instrumento, idn = correspondencias[0]
            if not explicito:
                instrumento.timeout = timeout_ms
            for detalhe in falhas:
                LOG.warning("autodeteccao ignorou %s", detalhe)
            serial = seriais.get(instrumento.device, "")
            LOG.info(
                "DHO804 selecionado em %s%s: %s",
                instrumento.device,
                f" (USB serial {serial})" if serial else "",
                idn,
            )
            entregue = instrumento
            return instrumento, idn

        detalhes = falhas + ignorados
        origem = os.fspath(device) if explicito else DEFAULT_DEVICE_GLOB
        sufixo = (
            "; ".join(detalhes) if detalhes else "nenhum candidato utilizavel"
        )
        raise UsbTmcDiscoveryError(
            f"nenhum DHO804 encontrado em {origem}: {sufixo}"
        )
    finally:
        for instrumento, _ in correspondencias:
            if instrumento is not entregue:
                instrumento.close()


def command_expects_reply(comando: bytes) -> bool:
    """Detecta '?' fora de strings SCPI, inclusive em comando composto."""
    aspas = None
    i = 0
    while i < len(comando):
        byte = comando[i]
        if aspas is not None:
            if byte == aspas:
                if i + 1 < len(comando) and comando[i + 1] == aspas:
                    i += 2
                    continue
                aspas = None
        elif byte in (ord("'"), ord('"')):
            aspas = byte
        elif byte == ord("?"):
            return True
        i += 1
    return False


def _send_all_interruptible(
    cliente: socket.socket,
    dados: bytes,
    stop_event: threading.Event,
) -> bool:
    """Envia uma resposta sem bloquear indefinidamente o encerramento."""
    restante = memoryview(dados)
    ultimo_progresso = time.monotonic()
    flags = getattr(socket, "MSG_DONTWAIT", 0)
    while restante:
        if stop_event.is_set():
            return False
        agora = time.monotonic()
        sem_progresso = agora - ultimo_progresso
        if sem_progresso >= CLIENT_SEND_STALL_SECONDS:
            raise TimeoutError(
                "cliente local nao consumiu a resposta SCPI em "
                f"{CLIENT_SEND_STALL_SECONDS:.1f} s"
            )
        _, gravaveis, _ = select.select(
            [],
            [cliente],
            [],
            min(
                CLIENT_POLL_SECONDS,
                CLIENT_SEND_STALL_SECONDS - sem_progresso,
            ),
        )
        if not gravaveis:
            continue
        try:
            enviados = cliente.send(
                restante[:CLIENT_SEND_CHUNK_SIZE], flags
            )
        except BlockingIOError:
            continue
        if enviados <= 0:
            raise ConnectionError("cliente local fechou durante a resposta")
        restante = restante[enviados:]
        ultimo_progresso = time.monotonic()
    return True


def _process_scpi_line(
    cliente: socket.socket,
    instrumento,
    linha: bytes,
    stop_event: threading.Event,
) -> bool:
    comando = linha.rstrip(b"\r")
    if not comando:
        return True
    instrumento.write_raw(comando + b"\n")
    if command_expects_reply(comando):
        resposta = instrumento.read_raw()
        return _send_all_interruptible(cliente, resposta, stop_event)
    return not stop_event.is_set()


def handle_scpi_client(
    cliente: socket.socket,
    instrumento,
    stop_event: Optional[threading.Event] = None,
) -> None:
    """Atende uma sessao de socket SCPI cru e obedece ao evento de parada."""
    parar = stop_event or threading.Event()
    if isinstance(instrumento, UsbTmcInstrument) and instrumento._native is not None:
        try:
            with instrumento._lock:
                instrumento._native.set_chunk(instrumento.chunk_size)
                instrumento._native.serve(cliente, parar)
        finally:
            cliente.close()
        return
    pendente = bytearray()
    try:
        while not parar.is_set():
            legiveis, _, _ = select.select(
                [cliente], [], [], CLIENT_POLL_SECONDS
            )
            if not legiveis:
                continue
            dados = cliente.recv(65536)
            if not dados:
                break
            pendente.extend(dados)
            while b"\n" in pendente and not parar.is_set():
                linha, _, resto = pendente.partition(b"\n")
                pendente = bytearray(resto)
                if not _process_scpi_line(
                        cliente, instrumento, bytes(linha), parar):
                    return
        if pendente and not parar.is_set():
            _process_scpi_line(
                cliente, instrumento, bytes(pendente), parar
            )
    finally:
        try:
            cliente.close()
        except OSError:
            pass


class ScpiUsbBridge:
    """Servidor de socket SCPI limitado ao loopback."""

    def __init__(
        self,
        instrumento: Optional[UsbTmcInstrument],
        host: str = DEFAULT_HOST,
        port: int = DEFAULT_PORT,
        instrument_factory=UsbTmcInstrument,
        *,
        instrument_discovery=None,
        discovery_attempts: int = 0,
        discovery_window_seconds: float = 0.0,
    ) -> None:
        if host not in ("127.0.0.1", "localhost", "::1"):
            raise ValueError("por seguranca, a ponte USB so pode ouvir em loopback")
        if instrumento is None and instrument_discovery is None:
            raise ValueError(
                "instrument_discovery e obrigatorio sem instrumento inicial")
        if discovery_attempts < 0:
            raise ValueError("discovery_attempts nao pode ser negativo")
        if discovery_attempts > 1 and discovery_window_seconds <= 0.0:
            raise ValueError(
                "discovery_window_seconds deve ser positivo com varias tentativas")
        self.instrumento = instrumento
        self.host = host
        self.port = int(port)
        self._instrument_factory = instrument_factory
        self._instrument_discovery = instrument_discovery
        self.discovery_attempts = int(discovery_attempts)
        self.discovery_window_seconds = float(discovery_window_seconds)

    @property
    def _discovery_interval_seconds(self) -> float:
        if self.discovery_attempts <= 1:
            return 0.0
        return self.discovery_window_seconds / (self.discovery_attempts - 1)

    def _forget_instrument(self) -> None:
        instrumento = self.instrumento
        self.instrumento = None
        if instrumento is not None:
            instrumento.close()

    def _discover_instrument(self, motivo: str) -> bool:
        if self.instrumento is not None:
            return True
        if self._instrument_discovery is None:
            LOG.warning("descoberta do DHO804 indisponivel (%s)", motivo)
            return False
        try:
            instrumento, idn = self._instrument_discovery()
        except UsbTmcError as exc:
            LOG.info("DHO804 ainda indisponivel (%s): %s", motivo, exc)
            notify_systemd(
                f"STATUS=Servico ativo; DHO804 indisponivel ({motivo})"
            )
            return False
        self.instrumento = instrumento
        LOG.info("DHO804 conectado (%s): %s", motivo, idn)
        notify_systemd(
            f"STATUS=Ponte USBTMC conectada em {instrumento.device}"
        )
        return True

    def _reopen_usb(self) -> None:
        """Fecha e reabre o descritor USBTMC entre duas sessoes TCP."""
        anterior = self.instrumento
        if anterior is None:
            raise UsbTmcDiscoveryError("DHO804 nao esta conectado")
        device = anterior.device
        timeout_ms = anterior.timeout
        chunk_size = anterior.chunk_size
        anterior.close()
        self.instrumento = self._instrument_factory(
            device=device,
            timeout_ms=timeout_ms,
            chunk_size=chunk_size,
        )
        LOG.info("descritor USBTMC reaberto em %s", device)

    def serve_forever(self, stop_event: Optional[threading.Event] = None) -> None:
        parar = stop_event or threading.Event()
        familia = socket.AF_INET6 if self.host == "::1" else socket.AF_INET
        tentativas_restantes = (
            self.discovery_attempts if self.instrumento is None else 0
        )
        numero_tentativa = 0
        proxima_tentativa = time.monotonic()
        try:
            with socket.socket(familia, socket.SOCK_STREAM) as servidor:
                servidor.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                servidor.bind((self.host, self.port))
                servidor.listen(1)
                servidor.settimeout(0.5)
                if self.instrumento is None:
                    LOG.info(
                        "servico pronto em %s:%d; aguardando DHO804",
                        self.host,
                        self.port,
                    )
                    status = "Servico ativo; aguardando DHO804"
                else:
                    LOG.info(
                        "ponte pronta em %s:%d -> %s",
                        self.host,
                        self.port,
                        self.instrumento.device,
                    )
                    status = (
                        f"Ponte USBTMC pronta em {self.host}:{self.port}"
                    )
                notify_systemd(f"READY=1\nSTATUS={status}")
                while not parar.is_set():
                    agora = time.monotonic()
                    if (self.instrumento is None and
                            tentativas_restantes > 0 and
                            agora >= proxima_tentativa):
                        numero_tentativa += 1
                        tentativas_restantes -= 1
                        motivo = (
                            f"tentativa automatica {numero_tentativa}/"
                            f"{self.discovery_attempts}"
                        )
                        if self._discover_instrument(motivo):
                            tentativas_restantes = 0
                        elif tentativas_restantes:
                            proxima_tentativa = (
                                time.monotonic()
                                + self._discovery_interval_seconds
                            )
                    try:
                        cliente, endereco = servidor.accept()
                    except socket.timeout:
                        continue
                    if (self.instrumento is None and
                            not self._discover_instrument(
                                "pedido do botao Conectar")):
                        LOG.info(
                            "conexao local recusada: DHO804 ainda indisponivel"
                        )
                        cliente.close()
                        continue
                    tentativas_restantes = 0
                    cliente.setsockopt(
                        socket.IPPROTO_TCP, socket.TCP_NODELAY, 1
                    )
                    LOG.info("cliente local conectado: %s", endereco)
                    falha_usb = False
                    try:
                        handle_scpi_client(cliente, self.instrumento, parar)
                    except UsbTmcError as exc:
                        falha_usb = True
                        LOG.warning(
                            "DHO804 desconectado durante a sessao: %s", exc
                        )
                    LOG.info("cliente local desconectado")
                    if falha_usb:
                        self._forget_instrument()
                        notify_systemd(
                            "STATUS=Servico ativo; aguardando novo Conectar"
                        )
                    elif not parar.is_set():
                        try:
                            self._reopen_usb()
                        except UsbTmcError as exc:
                            LOG.warning(
                                "DHO804 indisponivel ao reabrir USB: %s", exc
                            )
                            self._forget_instrument()
                            notify_systemd(
                                "STATUS=Servico ativo; aguardando novo Conectar"
                            )
        finally:
            self._forget_instrument()


def _argumentos(argv=None):
    parser = argparse.ArgumentParser(
        description="Ponte SCPI loopback para o DHO804 por USBTMC"
    )
    modo = parser.add_mutually_exclusive_group(required=True)
    modo.add_argument("--probe", action="store_true", help="consulta somente *IDN?")
    modo.add_argument("--serve", action="store_true", help="inicia a ponte socket")
    parser.add_argument(
        "--device",
        default=None,
        help="no USBTMC explicito; omitido, autodetecta o DHO804",
    )
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--timeout-ms", type=int, default=DEFAULT_TIMEOUT_MS)
    parser.add_argument(
        "--discovery-timeout-ms",
        type=int,
        default=DEFAULT_DISCOVERY_TIMEOUT_MS,
        help="prazo de *IDN? por candidato durante a autodeteccao",
    )
    parser.add_argument(
        "--discovery-attempts",
        type=int,
        default=0,
        help="tentativas automaticas opcionais antes do botao Conectar",
    )
    parser.add_argument(
        "--discovery-window-seconds",
        type=float,
        default=120.0,
        help="janela entre a primeira e a ultima tentativa automatica",
    )
    parser.add_argument("--log-level", default="INFO")
    return parser.parse_args(argv)


def main(argv=None) -> int:
    args = _argumentos(argv)
    logging.basicConfig(
        level=getattr(logging, args.log_level.upper(), logging.INFO),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    if args.probe:
        try:
            instrumento, idn = descobrir_dho804(
                device=args.device,
                timeout_ms=args.timeout_ms,
                discovery_timeout_ms=args.discovery_timeout_ms,
            )
        except UsbTmcError as exc:
            LOG.error("%s", exc)
            return 2
        with instrumento:
            print(idn)
            return 0

    if args.discovery_attempts < 0:
        LOG.error("--discovery-attempts nao pode ser negativo")
        return 2
    if (args.discovery_attempts > 1 and
            args.discovery_window_seconds <= 0.0):
        LOG.error("--discovery-window-seconds deve ser positivo")
        return 2

    parar = threading.Event()

    def encerrar(_sinal, _frame):
        parar.set()

    def descobrir():
        return descobrir_dho804(
            device=args.device,
            timeout_ms=args.timeout_ms,
            discovery_timeout_ms=args.discovery_timeout_ms,
        )

    signal.signal(signal.SIGINT, encerrar)
    signal.signal(signal.SIGTERM, encerrar)
    ScpiUsbBridge(
        None,
        args.host,
        args.port,
        instrument_discovery=descobrir,
        discovery_attempts=args.discovery_attempts,
        discovery_window_seconds=args.discovery_window_seconds,
    ).serve_forever(parar)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
