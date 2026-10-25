# Biblioteca C++ nativa

`transport.hpp` é uma biblioteca C++17 header-only. Inclua esse arquivo e ligue
com `-pthread`; não requer Python, Cython ou NumPy no runtime. O driver de kernel
Linux `usbtmc` continua responsável pelas transações USB.

```cpp
#include "transport.hpp"

int main() {
    dho804::Transport scope("/dev/usbtmc0", 5000, 4000);
    auto idn = scope.query("*IDN?\n");
    // O comando inclui o terminador. O objeto fecha o descritor via RAII.
}
```

Métodos: `write`, `query`, `read_raw`, `read_bytes`, `set_timeout`, `set_chunk`,
`closed`, `close` e `clear`. A aplicação deve manter o bloco USB em 4000 bytes
para o DHO804 validado. `clear` existe para compatibilidade e nunca é chamado
automaticamente: não o use para recuperar esse firmware.

Todas as operações de transporte são serializadas por mutex. `query` mantém
escrita/leitura na mesma seção crítica. O objeto deve continuar vivo enquanto
outras threads o utilizam. `close` é idempotente. Erros propagam exceções;
a biblioteca não repete consultas após timeout. `Backend` permite testes sem USB.

`serve_client(fd, transport, callback, contexto)` implementa uma sessão TCP:
comandos ASCII separados por LF, detecção de consultas respeitando aspas SCPI,
resposta binária integral, envio não bloqueante e interrupção entre operações.
O chamador possui e fecha o socket. O callback e seu contexto devem permanecer
válidos até o retorno. A espera de USB segue o timeout configurado; o envio
interrompe após 5 s sem progresso. O executável `bridge.cpp` adiciona descoberta
VID/PID + IDN, listener loopback, sinais e notificação systemd.

```sh
make -C native test
./build/bin/dho804-usb-native --serve --port 5556
```

A ponte Cython usa exatamente esse núcleo em `with nogil`; o callback de parada
reentra no GIL apenas para consultar o evento. A implementação com backend Python
injetado permanece exclusivamente para compatibilidade e testes, não sendo usada
pelo transporte real. A API Python pública continua disponível.
