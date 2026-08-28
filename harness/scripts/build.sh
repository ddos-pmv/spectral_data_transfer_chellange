#!/usr/bin/env bash
# Разовая установка DPDK и igb_uio на чистую Ubuntu 24.04. Идемпотентна.
#
#   sudo ./scripts/build.sh
#   sudo WITH_IGB_UIO=0 ./scripts/build.sh    # без ENA PMD, только AF_XDP
#
# Настройку машины это НЕ делает -- она в ./scripts/host.sh
set -euo pipefail
. "$(dirname "$(readlink -f "$0")")/lib.sh"

DPDK_REPO=${DPDK_REPO:-https://github.com/DPDK/dpdk-stable.git}
DPDK_VER=${DPDK_VER:-v25.11.2}
SRC=${SRC:-/opt/src}
WITH_IGB_UIO=${WITH_IGB_UIO:-1}
LIBDIR=/usr/local/lib/x86_64-linux-gnu

need_root

say "Пакеты"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq \
  build-essential git pkg-config meson ninja-build python3-pyelftools \
  libnuma-dev libbpf-dev libxdp-dev ethtool chrony pciutils \
  linux-headers-"$(uname -r)" linux-tools-"$(uname -r)" linux-tools-common

if pkg-config --exists libdpdk 2>/dev/null; then
  say "DPDK уже установлен: $(pkg-config --modversion libdpdk)"
else
  say "Сборка DPDK $DPDK_VER (несколько минут)"
  mkdir -p "$SRC" && cd "$SRC"
  [ -d dpdk-stable ] || git clone --quiet "$DPDK_REPO" dpdk-stable
  cd dpdk-stable
  git fetch --quiet --tags
  git checkout --quiet "$DPDK_VER"
  # Драйвер net_ena собирается отдельным .so и подгружается
  # EAL в рантайме -- поэтому важна и сборка, и ldconfig ниже.
  meson setup build --prefix=/usr/local -Dplatform=native -Dbuildtype=release
  ninja -C build
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
     make -j$(nproc)                          собрать проект
     cp scripts/host.conf.example scripts/host.conf && $EDITOR scripts/host.conf
     sudo ./scripts/host.sh apply             настроить машину
     ./scripts/checkenv.sh                    что осталось (GRUB, часы)
NEXT
