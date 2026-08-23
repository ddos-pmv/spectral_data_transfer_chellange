# Адреса ENI, отданной DPDK. Подключается через `source` из tx.sh и rx.sh.
#
# В режиме ENA PMD карта отвязана от ядра, поэтому ip/ethtool про неё ничего не
# знают. Единственный источник её MAC и IP внутри инстанса — метаданные EC2.

IMDS_TOKEN=""

# imds <путь> — GET по метаданным. IMDSv2: сначала берём токен, он живёт 5 минут.
imds() {
  [ -n "$IMDS_TOKEN" ] || IMDS_TOKEN=$(curl -s -X PUT \
      -H 'X-aws-ec2-metadata-token-ttl-seconds: 300' \
      http://169.254.169.254/latest/api/token)
  curl -s -H "X-aws-ec2-metadata-token: $IMDS_TOKEN" \
      "http://169.254.169.254/latest/meta-data/$1"
}

# eni_addrs <device-index> — печатает "MAC IP" нужной ENI.
# device-index 0 — первичная (SSH), 1 — та, что мы приаттачили под DPDK.
eni_addrs() {
  local want=$1 mac
  for mac in $(imds network/interfaces/macs/); do
    mac=${mac%/}
    [ "$(imds "network/interfaces/macs/$mac/device-number")" = "$want" ] || continue
    echo "$mac $(imds "network/interfaces/macs/$mac/local-ipv4s" | head -1)"
    return 0
  done
  return 1
}

# Режим ядра (AF_XDP поверх живого интерфейса) или режим PMD (карта у DPDK).
kernel_mode() { case "$EAL_DEV" in *net_af_xdp*) return 0;; *) return 1;; esac; }
