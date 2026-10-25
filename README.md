# Rigol DHO804 Lxi via USBTMC Driver

Migração do driver customizado do Rigol DHO804 para C++17, com interface Cython
compatível com os aplicativos existentes. O transporte real não usa operações
Python para abrir/ler/escrever USB, montar blocos IEEE ou processar os dados TCP.
A ponte independente também implementa descoberta, listener, sinais e systemd
em C++ e não depende de Python no runtime.

## Organização

- `native/transport.hpp`: biblioteca C++ header-only, transporte POSIX, mutex,
  buffers, parser IEEE e sessão TCP. Consulte `native/README.md` para uso direto.
- `native/bridge.cpp`: executável independente `dho804-usb-native`.
- `src/dho804_usb/driver.pyx`: API Cython; delega o caminho real ao C++ com GIL
  liberado. Mantém CLI/descoberta de integração e backends Python injetáveis para
  compatibilidade/testes. Não existe fallback de transporte POSIX em Python.
- `examples/bench_bridge.cpp`: captura integral e medição em C++.
- `native/test_transport.cpp`, `tests/`: testes nativos e de compatibilidade.
- `build/cython/`: C++/HTML gerados pelo Cython; nunca editar diretamente.
- `validation/`: evidências de bancada. As capturas brutas permanecem locais.

## Dependências e compilação

Fedora, compilador C++17 e headers Python para a extensão. Pacotes de validação
instalados nesta máquina: `libasan`, `libubsan`, `libtsan`, `cppcheck` e
`clang-tools-extra`. Python 3.14.8, Cython 3.2.4, setuptools 80.10.2 e GCC 16.2.1
foram usados. Cython requer >=3.2,<4; setuptools >=77.

```sh
make -C native test
python3 setup.py build_ext --inplace
python3 -m pip wheel --no-deps --no-build-isolation --no-index . -w dist
```

O executável está em `build/bin/dho804-usb-native`. O wheel 0.3.0 nos anexos da release é
específico para CPython 3.14/Linux x86_64. Recompile na CNC para o Python e as
bibliotecas C/C++ dela; não copie cegamente os binários desta máquina.
O pacote de fontes da release inclui o núcleo C++ e seu Makefile.

## Uso

Conecte somente o instrumento pretendido, configure a memória e conceda ao
usuário leitura/escrita em `/dev/usbtmc0`. Use apenas uma ponte por vez:

```sh
./build/bin/dho804-usb-native --serve --port 5556
# Alternativa compatível com Python, usando o mesmo núcleo C++:
PYTHONPATH=src python3 -m dho804_usb --serve --port 5556
```

A descoberta confirma VID:PID 1ab1:044d e `*IDN?`. O listener aceita somente
loopback; `--device /dev/usbtmc0` permite selecionar explicitamente o aparelho.
Sem cliente e sem tentativas automáticas configuradas, não acessa USB.

Em outro terminal, compile o cliente e capture toda a memória atual do CH1:

```sh
mkdir -p build/bin captures
g++ -O2 -std=c++17 -Wall -Wextra -Wpedantic -pthread examples/bench_bridge.cpp -o build/bin/bench_bridge
./build/bin/bench_bridge 5556 captures/nova_captura_ch1
```

O cliente confirma RAM/RAW/WORD e exige exatamente dois bytes por ponto. Usa
uma única consulta de forma de onda para a RAM integral. Salva `.raw` uint16
little-endian e `.txt` com preâmbulo e tempos, sem sobrescrever arquivos.
Para a aquisição e restaura RUN após sucesso se estava rodando.

O bloco de leitura USB continua em **4000 bytes**; isso não limita a janela a
4000/5000 pontos. Leituras maiores truncaram respostas neste firmware. A ponte
monta a resposta integral antes de enviar ao cliente (timeout do exemplo: 120 s).
Não repete consulta após falha nem executa CLEAR automaticamente. Não use CLEAR
como recuperação: esse comando já travou o aparelho em bancada.

## Validação real — 25/10/2026

DHO804 firmware 00.01.05, CH1 RAW/WORD, **25.000.000 pontos, 50.000.000 bytes,
uma janela** em cada captura. Fedora kernel 7.2.11-200.fc44.x86_64, i5-13500.

| Medida | Ponte C++ independente | Interface Cython + núcleo C++ |
| --- | ---: | ---: |
| Transferência | 15,731190 s | 14,980944 s |
| Total do cliente | 16,277668 s | 15,738075 s |
| CPU da ponte durante o cliente | 0,27 s | 0,28 s |
| CPU do cliente | 0,006523 s | 0,005980 s |
| Tempo dentro de recv | 15,731081 s | 14,980848 s |
| Gravação com fsync | 0,087530 s | 0,087467 s |

A versão 0.2.0 levou 15,756447 s nesta máquina. São execuções isoladas, não um
benchmark estatístico; não demonstram ganho causal. Os 11,939663 s anteriores
foram na CNC com outro hardware/kernel. Jitter e iowait do sistema não foram
medidos; tempo dentro de recv não equivale ao iowait do sistema.

`validation/capture-{native,cython}-v03.json` registra contagens, preâmbulo,
tempos e SHA-256. Os arquivos `.raw` de 50 MB permanecem na bancada e não são distribuídos.
Os checksums e as faixas das amostras estão em `capture-v03-integrity.json`.

## Testes e análise

```sh
PYTHONPATH=src python3 -m unittest discover -s tests -v
make -C native test
make -C native asan
make -C native tsan
./build/bin/bench_bridge --self-test
```

Passaram 13 testes do núcleo C++, 14 testes da interface Cython e 5 do cliente.
O núcleo cobre 50 MB completos, fragmentação, truncamento, timeout sem retry,
leituras limitadas, escritas parciais, mutex entre threads, sessão TCP, parada
com cliente lento e desconexão sem SIGPIPE. O CLI foi verificado sem USB quanto
a argumentos inválidos e encerramento por SIGTERM.

ASan+UBSan e TSan passaram separadamente no núcleo e na extensão. LeakSanitizer
ficou ativo no executável de testes nativos; na extensão foi desabilitado para
não atribuir alocações do interpretador Python ao módulo. Nenhuma supressão de
relatórios de race/ASan foi adicionada. Cppcheck e verificações clang-analyzer
selecionadas do clang-tidy foram executados no C++ escrito à mão. Os logs atuais
estão em `validation/native-*.log`, `extension-*.log`, `cppcheck-native.log` e
`clang-tidy-native.log`. As bibliotecas de sanitizer ausentes no primeiro ensaio foram instaladas antes
da validação desta versão.

Os hashes dos binários usados na bancada estão em `validation/benchmark-build-info.json`.
Os anexos publicados têm seus próprios hashes em `SHA256SUMS` na release.
O executável Linux x86_64 foi compilado no Fedora 44; para outra distribuição,
prefira recompilar os fontes. As capturas não são necessárias para executar os testes.
