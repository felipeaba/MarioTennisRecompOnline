# Recompilando a edição de teste

O código em MarioTennisRecomp é o port de Ieatzcrayonz na revisão
856e128e319dc1bc3a4f6d726bf0402e532bfadc, com as mudanças da branch
feature/netplay aplicadas. A pasta recomp-net contém a revisão
e45ad4a384fa78c4c3cd9d828bad39453c2c42b6. O executável usa recompilação
em tempo de execução. O build em si não precisa da ROM.

- Base do port: https://github.com/Ieatzcrayonz/MarioTennisRecomp/tree/856e128e319dc1bc3a4f6d726bf0402e532bfadc
- recomp-net: https://github.com/RetroPortingToolKit/recomp-net/tree/e45ad4a384fa78c4c3cd9d828bad39453c2c42b6
- Bibliotecas e submódulos: https://github.com/Zelda64Recomp/Zelda64Recomp/tree/b65c482ed672258eb684ab94a4e8c8aa662615ee

No Developer PowerShell x64 do Visual Studio, com Git, CMake, Ninja, clang-cl
e o Windows SDK, execute a partir da pasta que cont?m o checkout MarioTennisRecomp.
A dependência SDL2 2.30.3 é configurada pelo CMake do port.

```powershell
git clone https://github.com/RetroPortingToolKit/recomp-net.git
git -C recomp-net checkout e45ad4a384fa78c4c3cd9d828bad39453c2c42b6
git clone https://github.com/Zelda64Recomp/Zelda64Recomp.git
git -C Zelda64Recomp checkout b65c482ed672258eb684ab94a4e8c8aa662615ee
git -c core.longpaths=true -C Zelda64Recomp submodule update --init --recursive
$port = (Resolve-Path .\MarioTennisRecomp).Path
$libs = (Resolve-Path .\Zelda64Recomp\lib).Path
New-Item -ItemType Junction -Path "$port\lib" -Target $libs
git -C "$libs\N64ModernRuntime" apply "$port\patches\n64modernruntime-mario-tennis.patch"
git -C "$libs\N64ModernRuntime\N64Recomp" apply "$port\patches\n64recomp-mario-tennis.patch"
git -c core.autocrlf=false -C "$libs\rt64" apply --ignore-space-change "$port\patches\rt64-mario-tennis.patch"
$net = (Resolve-Path .\recomp-net).Path
git -C "$net" apply "$port\patches\recomp-net-lan-pacing.patch"
git -C "$net" apply "$port\patches\recomp-net-lan-reservations.patch"
cmake -S "$port" -B .\build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DMT_LIVE_RECOMP=ON "-DMT_RECOMP_NET_SOURCE_DIR=$net"
cmake --build .\build --target MarioTennisRecompiled
```

O port original detalha as ferramentas, a configuração e os créditos no README.
Os binários app-local do Visual C++ vêm do diretório de redistribuição do
Visual Studio Build Tools 2022. Os binários DXC vêm do submódulo RT64/dxc-bin.

O launcher tem código C# e XAML em MarioTennisRecomp/tools/lan_launcher.
De um PowerShell com o compilador .NET Framework do Windows disponível,
execute o script build.ps1 desse diretório com -Output e o caminho do EXE.
O launcher espera o executável do jogo, portable.txt, DLLs, assets e a
ROM já importada na subpasta jogo, ao lado do próprio launcher.

## Testes de duplas

A linha de replay contém o número do quadro, seguido de botões, X e Y
por porta. Em duplas são quatro trios. Os dois primeiros quadros precisam
ser neutros para respeitar o atraso de entrada.

```powershell
$build = 'C:\\caminho\\do\\build'
python tools/make_doubles_replay.py doubles.txt
python tools/test_netplay_doubles.py $build doubles.txt '2+2' logs-pair --require-point
python tools/test_netplay_doubles.py $build doubles.txt '1+1+1+1' logs-hub --require-point --reject-duplicate
```

O teste de quatro máquinas pode tentar conectar um segundo cliente na vaga P2.
O host rejeita a tentativa e preserva a vaga da primeira conexão. Para
simular rede, execute em um terminal de teste separado com
RNET_SIM_LOSS_PCT=5, RNET_SIM_LATENCY_MS=10 e RNET_SIM_JITTER_MS=3.
