#!/usr/bin/env bash
# Установка DPDK и всего, что нужно для прогона, на ЧИСТУЮ Ubuntu 24.04 (AWS).
# Запускать на КАЖДОМ из двух инстансов:
#
#   sudo ./scripts/aws-setup.sh
#   sudo WITH_IGB_UIO=0 ./scripts/aws-setup.sh     # только AF_XDP, без ENA PMD
#
# Повторный запуск пропускает уже сделанное.
set -euo pipefail

DPDK_REPO=${DPDK_REPO:-https://github.com/DPDK/dpdk-stable.git}
DPDK_VER=${DPDK_VER:-v25.11.2}
SRC=${SRC:-/opt/src}
HUGEPAGES_2M=${HUGEPAGES_2M:-2048}      # 4 ГБ; страницы по 1 ГБ — только через GRUB, см. AWS.md
WITH_IGB_UIO=${WITH_IGB_UIO:-1}         # igb_uio с Write Combining — нужен для LLQ на ENA
LIBDIR=/usr/local/lib/x86_64-linux-gnu

[ "$(id -u)" = 0 ] || { echo "запускать через sudo"; exit 1; }
step() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }

# ---------------------------------------------------------------- пакеты
step "Пакеты"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq \
  build-essential git pkg-config meson ninja-build python3-pyelftools \
  libnuma-dev libbpf-dev libxdp-dev ethtool chrony \
  linux-headers-"$(uname -r)" linux-tools-"$(uname -r)" linux-tools-common

# ------------------------------------------------------------------ DPDK
if pkg-config --exists libdpdk 2>/dev/null; then
  step "DPDK уже установлен: $(pkg-config --modversion libdpdk) — пропускаю сборку"
else
  step "Сборка DPDK $DPDK_VER (несколько минут)"
  mkdir -p "$SRC" && cd "$SRC"
  [ -d dpdk-stable ] || git clone --quiet "$DPDK_REPO" dpdk-stable
  cd dpdk-stable
  git fetch --quiet --tags
  git checkout --quiet "$DPDK_VER"
  # Драйверы (net_ena, net_af_xdp) собираются как отдельные .so и подгружаются
  # EAL в рантайме — поэтому важна и сборка, и ldconfig ниже.
  meson setup build --prefix=/usr/local -Dplatform=native -Dbuildtype=release
  ninja -C build
  ninja -C build install
fi

step "Регистрация библиотек в ldconfig"
echo "$LIBDIR" > /etc/ld.so.conf.d/dpdk.conf
ldconfig
PMDS=$(find "$LIBDIR" -maxdepth 2 -type d -name 'pmds-*' | head -1)
echo "драйверы: $PMDS"
ls "$PMDS" | grep -E 'net_(ena|af_xdp)\.so$' || {
  echo "ОШИБКА: нет net_ena/net_af_xdp — без них vdev не поднимется"; exit 1; }

# ------------------------------------------------------------- hugepages
step "Hugepages 2M: $HUGEPAGES_2M"
echo "$HUGEPAGES_2M" > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
grep -E 'HugePages_(Total|Free)' /proc/meminfo

# --------------------------------------------------------------- igb_uio
if [ "$WITH_IGB_UIO" = 1 ]; then
  step "igb_uio с Write Combining (нужен для LLQ на ENA)"
  cd "$SRC"
  [ -d dpdk-kmods ] || git clone --quiet git://dpdk.org/dpdk-kmods
  cd dpdk-kmods/linux/igb_uio && make >/dev/null
  install -D -m 644 igb_uio.ko "/lib/modules/$(uname -r)/extra/igb_uio.ko"
  depmod -a
  # wc_activate=1 обязателен: без Write Combining LLQ упирается в медленные
  # PCI-транзакции, и производительность падает вместо того чтобы вырасти.
  echo "options igb_uio wc_activate=1" > /etc/modprobe.d/igb_uio.conf
  modprobe uio
  modprobe igb_uio || insmod ./igb_uio.ko wc_activate=1
  grep -q igb_uio /proc/modules && echo "igb_uio загружен"
fi

# ---------------------------------------------------------------- итоги
step "Готово"
echo "DPDK      : $(pkg-config --modversion libdpdk)"
echo "интерфейсы: $(ls /sys/class/net | tr '\n' ' ')"
cat <<'NEXT'

Дальше:
  1. make -j$(nproc)                      собрать проект
  2. ./scripts/checkenv.sh                посмотреть, что ещё не настроено
  3. AWS.md, разделы 4-5                  GRUB, прерывания, часы (нужен reboot)
  4. AWS.md, раздел 7 или 8               запуск через AF_XDP или ENA PMD
NEXT
