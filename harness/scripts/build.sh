#!/usr/bin/env bash
# Разовая установка DPDK и igb_uio на чистую Ubuntu 24.04. Идемпотентна.
#
#   sudo ./scripts/build.sh
#   sudo WITH_IGB_UIO=0 ./scripts/build.sh   # не собирать igb_uio
#   sudo REBUILD=1 ./scripts/build.sh        # пересобрать, даже если DPDK стоит
#
#   # локальный функциональный тест на veth (см. veth_setup.sh), без карты:
#   sudo WITH_IGB_UIO=0 EXTRA_DRIVERS=net/af_packet REBUILD=1 ./scripts/build.sh
#
# REBUILD=1 нужен после СМЕНЫ ТИПА ИНСТАНСА. Сборка идёт с -Dplatform=native,
# то есть под конкретный CPU, и при переезде (скажем, Sapphire Rapids -> Ice
# Lake) старые бинарники падают по SIGILL. Обычная проверка "DPDK уже стоит"
# в этом случае вредна: она пропустит пересборку, а pkg-config продолжит
# отдавать -march прошлой машины и в harness.
#
# Настройку машины это НЕ делает -- она в ./scripts/host.sh
set -euo pipefail
. "$(dirname "$(readlink -f "$0")")/lib.sh"

DPDK_REPO=${DPDK_REPO:-https://github.com/DPDK/dpdk-stable.git}
DPDK_VER=${DPDK_VER:-v25.11.2}
SRC=${SRC:-/opt/src}
WITH_IGB_UIO=${WITH_IGB_UIO:-1}
# Пусто -- только net/ena (прод). net/af_packet -- для локального теста на
# veth, реальной карте он не нужен и в обычную сборку не идёт.
EXTRA_DRIVERS=${EXTRA_DRIVERS:-}
LIBDIR=/usr/local/lib/x86_64-linux-gnu

need_root

say "Пакеты"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq \
  build-essential git pkg-config meson ninja-build python3-pyelftools \
  libnuma-dev libbpf-dev libxdp-dev ethtool chrony pciutils \
  linux-headers-"$(uname -r)" linux-tools-"$(uname -r)" linux-tools-common

REBUILD=${REBUILD:-0}
if [ "$REBUILD" = 0 ] && pkg-config --exists libdpdk 2>/dev/null; then
  say "DPDK уже установлен: $(pkg-config --modversion libdpdk)"
  say "  (сменили тип инстанса? нужен sudo REBUILD=1 ./scripts/build.sh)"
else
  say "Сборка DPDK $DPDK_VER (несколько минут)"
  mkdir -p "$SRC" && cd "$SRC"
  [ -d dpdk-stable ] || git clone --quiet "$DPDK_REPO" dpdk-stable
  cd dpdk-stable
  git fetch --quiet --tags
  git checkout --quiet "$DPDK_VER"

  # Каталог сборки сносится: meson кэширует -march, и после смены типа
  # инстанса переконфигурация поверх старого build даёт флаги прошлой машины.
  rm -rf build

  # Только net_ena: решение работает на ENA и ни на чём другом, а каждый лишний
  # драйвер -- это и время сборки, и лишний .so, который EAL будет открывать.
  # EXTRA_DRIVERS добавляет к этому списку явно (для локального теста на veth,
  # net/af_packet) -- по умолчанию пусто, прод-сборку не трогает.
  DRIVERS=net/ena
  [ -n "$EXTRA_DRIVERS" ] && DRIVERS="$DRIVERS,$EXTRA_DRIVERS"
  meson setup build --prefix=/usr/local -Dplatform=native -Dbuildtype=release \
        -Denable_drivers="$DRIVERS" -Dtests=false
  ninja -C build

  # Старые PMD сносятся ПОСЛЕ успешной сборки и перед установкой, а не раньше:
  # упавшая сборка не должна оставить машину вообще без драйверов.
  #
  # Сносить обязательно, потому что `ninja install` старые .so не удаляет --
  # только докладывает новые, -- а EAL при старте делает dlopen ВСЕГО, что
  # найдёт в pmds-*. После пересборки под другой CPU один залежавшийся драйвер
  # уронит процесс по SIGILL прямо в rte_eal_init, ещё до нашего кода, и
  # выглядеть это будет необъяснимо.
  rm -rf "$LIBDIR"/dpdk/pmds-*

  ninja -C build install
fi

say "Регистрация библиотек"
echo "$LIBDIR" > /etc/ld.so.conf.d/dpdk.conf
ldconfig
PMDS=$(find "$LIBDIR" -maxdepth 2 -type d -name 'pmds-*' | head -1)
row "драйверы" "$PMDS"
ls "$PMDS" | grep -E 'net_ena\.so$' | sed 's/^/   /' \
  || die "нет net_ena -- без него порт не поднимется"

if [ "$WITH_IGB_UIO" = 1 ]; then
  say "igb_uio с Write Combining"
  cd "$SRC"
  # https, а не git:// -- порт 9418 в AWS обычно закрыт
  [ -d dpdk-kmods ] || git clone --quiet https://dpdk.org/git/dpdk-kmods
  cd dpdk-kmods/linux/igb_uio && make >/dev/null
  install -D -m 644 igb_uio.ko "/lib/modules/$(uname -r)/extra/igb_uio.ko"
  depmod -a
  # Без WC каждая запись LLQ-дескриптора уезжает отдельной PCIe-транзакцией.
  # Проверить, что WC реально активен: ./scripts/nic.sh status во время прогона.
  ensure_igb_uio
fi

say "Готово"
row "DPDK" "$(pkg-config --modversion libdpdk)"
cat <<'NEXT'

   Дальше:
     cmake -S . -B build                      сконфигурировать (разово)
     cmake --build build -j$(nproc)            собрать проект
     cp scripts/host.conf.example scripts/host.conf && $EDITOR scripts/host.conf
     sudo ./scripts/host.sh apply             настроить машину
     ./scripts/checkenv.sh                    что осталось (GRUB, часы)
NEXT
