#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_ether.h>

#include <algorithm>
#include <vector>
#include <array>
#include <string>
#include <string_view>

#include <cstdio>

struct OffloadInfo {
    uint64_t flag;
    std::string_view name;
    std::string_view description;
};

void print_rx_offloads(uint64_t capa, uint64_t enabled) {
    constexpr std::array rx_offloads_list = {
       OffloadInfo{RTE_ETH_RX_OFFLOAD_VLAN_STRIP,    "VLAN_STRIP",    "Аппаратное вырезание VLAN-тега"},
        OffloadInfo{RTE_ETH_RX_OFFLOAD_IPV4_CKSUM,    "IPV4_CKSUM",    "Проверка контрольной суммы IPv4"},
        OffloadInfo{RTE_ETH_RX_OFFLOAD_UDP_CKSUM,     "UDP_CKSUM",     "Проверка контрольной суммы UDP"},
        OffloadInfo{RTE_ETH_RX_OFFLOAD_TCP_CKSUM,     "TCP_CKSUM",     "Проверка контрольной суммы TCP"},
        OffloadInfo{RTE_ETH_RX_OFFLOAD_TCP_LRO,       "TCP_LRO",       "Large Receive Offload (сборка пакетов L4)"},
        OffloadInfo{RTE_ETH_RX_OFFLOAD_QINQ_STRIP,    "QINQ_STRIP",    "Вырезание двойного VLAN (QinQ)"},
        OffloadInfo{RTE_ETH_RX_OFFLOAD_OUTER_IPV4_CKSUM, "OUTER_IPV4", "Чексумма внешнего IP-заголовка (для туннелей)"},
        OffloadInfo{RTE_ETH_RX_OFFLOAD_MACSEC_STRIP,  "MACSEC_STRIP",  "Аппаратное снятие защиты MACsec"},
        OffloadInfo{RTE_ETH_RX_OFFLOAD_VLAN_FILTER,   "VLAN_FILTER",   "Аппаратная фильтрация VLAN"},
        OffloadInfo{RTE_ETH_RX_OFFLOAD_VLAN_EXTEND,   "VLAN_EXTEND",   "Расширенная обработка VLAN (QinQ)"},
        OffloadInfo{RTE_ETH_RX_OFFLOAD_SCATTER,       "SCATTER",       "Сборка пакета в цепочку буферов (Chained mbufs)"},
        OffloadInfo{RTE_ETH_RX_OFFLOAD_TIMESTAMP,     "TIMESTAMP",     "Аппаратные наносекундные метки времени"},
        OffloadInfo{RTE_ETH_RX_OFFLOAD_KEEP_CRC,      "KEEP_CRC",      "Сохранение FCS/CRC Ethernet-кадра в буфере"},
        OffloadInfo{RTE_ETH_RX_OFFLOAD_SCTP_CKSUM,    "SCTP_CKSUM",    "Проверка контрольной суммы SCTP"},
        OffloadInfo{RTE_ETH_RX_OFFLOAD_OUTER_UDP_CKSUM, "OUTER_UDP",   "Чексумма внешнего UDP-заголовка"}
    };

    std::fprintf(stdout, "========================================================================\n");
    std::fprintf(stdout, "%-20s | %-12s | %-10s | %s\n", "НАЗВАНИЕ ОФФЛОАДА", "ПОДДЕРЖКА", "СТАТУС", "ОПИСАНИЕ");
    std::fprintf(stdout, "========================================================================\n");

    for (const auto& offload : rx_offloads_list) {
        bool is_supported = (capa & offload.flag) != 0;
        bool is_enabled = (enabled & offload.flag) != 0;

        const char* support_str = is_supported ? " + " : " - ";
        const char* status_str = "-";

        if (is_supported) {
            status_str = is_enabled ? " ON " : " OFF ";
        }

        std::fprintf(stdout, "%-20s | %-12s | %-10s | %s\n",
                     offload.name.data(),
                     support_str,
                     status_str,
                     offload.description.data());
    }
    std::fprintf(stdout, "========================================================================\n");
}


void print_tx_offloads(uint64_t capa, uint64_t enabled) {
    constexpr std::array tx_offloads_list = {
        OffloadInfo{RTE_ETH_TX_OFFLOAD_VLAN_INSERT,    "VLAN_INSERT",   "Аппаратная вставка VLAN-тега"},
        OffloadInfo{RTE_ETH_TX_OFFLOAD_IPV4_CKSUM,     "IPV4_CKSUM",    "Генерация контрольной суммы IPv4"},
        OffloadInfo{RTE_ETH_TX_OFFLOAD_UDP_CKSUM,      "UDP_CKSUM",     "Генерация контрольной суммы UDP"},
        OffloadInfo{RTE_ETH_TX_OFFLOAD_TCP_CKSUM,      "TCP_CKSUM",     "Генерация контрольной суммы TCP"},
        OffloadInfo{RTE_ETH_TX_OFFLOAD_SCTP_CKSUM,     "SCTP_CKSUM",    "Генерация контрольной суммы SCTP"},
        OffloadInfo{RTE_ETH_TX_OFFLOAD_TCP_TSO,        "TCP_TSO",       "TCP Segmentation Offload (нарезка больших пакетов)"},
        OffloadInfo{RTE_ETH_TX_OFFLOAD_UDP_TSO,        "UDP_TSO",       "UDP Segmentation Offload"},
        OffloadInfo{RTE_ETH_TX_OFFLOAD_OUTER_IPV4_CKSUM,"OUTER_IPV4",    "Чексумма внешнего IPv4 для туннелей"},
        OffloadInfo{RTE_ETH_TX_OFFLOAD_QINQ_INSERT,    "QINQ_INSERT",   "Вставка двойного VLAN (QinQ)"},
        OffloadInfo{RTE_ETH_TX_OFFLOAD_VXLAN_TNL_TSO,  "VXLAN_TSO",     "TSO внутри туннелей VXLAN"},
        OffloadInfo{RTE_ETH_TX_OFFLOAD_GRE_TNL_TSO,    "GRE_TSO",       "TSO внутри туннелей GRE"},
        OffloadInfo{RTE_ETH_TX_OFFLOAD_MACSEC_INSERT,  "MACSEC_INSERT", "Аппаратная защита MACsec"},
        OffloadInfo{RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE, "FAST_FREE",     "Быстрое освобождение mbuf (без сверки рефкаунтов)"},
        OffloadInfo{RTE_ETH_TX_OFFLOAD_SECURITY,       "SECURITY",      "Аппаратное шифрование (например, IPsec)"},
        OffloadInfo{RTE_ETH_TX_OFFLOAD_OUTER_UDP_CKSUM,"OUTER_UDP",     "Чексумма внешнего UDP для туннелей"}
    };

    std::fprintf(stdout, "========================================================================\n");
    std::fprintf(stdout, "%-20s | %-12s | %-10s | %s\n", "НАЗВАНИЕ TX ОФФЛОАДА", "ПОДДЕРЖКА", "СТАТУС", "ОПИСАНИЕ");
    std::fprintf(stdout, "========================================================================\n");

    for (const auto& offload : tx_offloads_list) {
        bool is_supported = (capa & offload.flag) != 0;
        bool is_enabled = (enabled & offload.flag) != 0;

        const char* support_str = is_supported ? " + " : " - ";
        const char* status_str = "-";

        if (is_supported) {
            status_str = is_enabled ? " ON " : " OFF ";
        }

        std::fprintf(stdout, "%-20s | %-12s | %-10s | %s\n",
                     offload.name.data(), support_str, status_str, offload.description.data());
    }
    std::fprintf(stdout, "========================================================================\n");
}

int main(int argc, char** argv) {
    if (rte_eal_init(argc, argv) < 0) {
        rte_exit(EXIT_FAILURE, "rte_eal_init failed: %s\n",
                 rte_strerror(rte_errno));
    }
    uint16_t avail_cnt = rte_eth_dev_count_avail();
    std::fprintf(stdout, "rte_eth_dev_count_avail()=%d\n", avail_cnt);

    if (avail_cnt == 0) {
        std::fprintf(stderr, "Error: no available Ethernet devices\n");
    }

    uint16_t port_id;
    bool found_port = false;
    RTE_ETH_FOREACH_DEV(port_id) {
        struct rte_eth_dev_info dev_info;

        int ret = rte_eth_dev_info_get(port_id, &dev_info);
        if (ret != 0) {
            std::fprintf(stderr, "Ошибка получения инфо для порта %u: %s\n",
                         port_id, rte_strerror(-ret));
            continue;
        }

        std::fprintf(stdout, "\n--- Порт %u ---\n", port_id);
        std::fprintf(stdout, "  Драйвер: %s\n", dev_info.driver_name);
        std::fprintf(stdout, "  Макс RX очередей: %u\n", dev_info.max_rx_queues);
        std::fprintf(stdout, "  Макс TX очередей: %u\n", dev_info.max_tx_queues);
        std::fprintf(stdout, "  Имя PCI устройства: %s\n", rte_dev_name(dev_info.device));

        struct rte_ether_addr mac;
        ret = rte_eth_macaddr_get(port_id, &mac);
        if (ret != 0) {
            std::fprintf(stderr, "Error of getting macaddro for port %u: %s\n", port_id, rte_strerror(-ret));
        }

        std::fprintf(stdout, "  MAC-адрес: " RTE_ETHER_ADDR_PRT_FMT "\n\n",
                     RTE_ETHER_ADDR_BYTES(&mac));

        print_tx_offloads(dev_info.tx_offload_capa, dev_info.default_txconf.offloads);
        print_rx_offloads(dev_info.rx_offload_capa, dev_info.default_rxconf.offloads);

        printf("tx_offload_capa = 0x%016lx\n", dev_info.tx_offload_capa);
        printf("rx_offload_capa = 0x%016lx\n", dev_info.rx_offload_capa);
        printf("MULTI_SEGS: %s\n",
               (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MULTI_SEGS) ? "да" : "нет");

        found_port = true;
        break;

    }

    if (!found_port) {
        std::printf("Didn't find port");
        std::exit(1);
    }

    struct rte_mempool *mempool = rte_pktmbuf_pool_create("probe_pool", 8191, 256, 0, RTE_MBUF_DEFAULT_BUF_SIZE,rte_socket_id());

    struct rte_eth_conf eth_conf{};
    int ret = rte_eth_dev_configure(port_id, 1,1, &eth_conf);
    if (ret != 0) {
        std::fprintf(stderr, "Error: failed to configure port %u: %s\n", port_id, rte_strerror(-ret));
    }

    uint16_t nb_rx = 1024, nb_tx = 1024;
    rte_eth_dev_adjust_nb_rx_tx_desc(port_id, &nb_rx, &nb_tx);
    rte_eth_rx_queue_setup(port_id, 0, nb_rx, rte_socket_id(), nullptr, mempool);
    rte_eth_tx_queue_setup(port_id, 0, nb_tx, rte_socket_id(), nullptr);


    rte_eth_dev_start(port_id);

    struct rte_eth_link link{};
    int link_ret = rte_eth_link_get(port_id, &link);
    if (link_ret != 0) {
        std::fprintf(stderr, "rte_eth_link_get failed: %s\n",
                     rte_strerror(-link_ret));
        return 1;
    }
    std::fprintf(stdout, "\n  Линк: %s, скорость %u Mbps, %s\n",
                 link.link_status == RTE_ETH_LINK_UP ? "up" : "down",
                 link.link_speed,
                 link.link_duplex == RTE_ETH_LINK_FULL_DUPLEX ? "full-duplex"
                                                              : "half-duplex");
    return 0;
}