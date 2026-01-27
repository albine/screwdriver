/**
 * mmap_to_clickhouse - Fast C++ tool to export mmap binary files to TSV for ClickHouse import
 *
 * Usage:
 *   ./build/mmap_to_clickhouse --type orders /path/to/orders.bin | \
 *       clickhouse-client --query "INSERT INTO MDOrderStruct FORMAT TabSeparated"
 *
 * Supports:
 *   - orders.bin -> MDOrderStruct (144 bytes per record)
 *   - transactions.bin -> MDTransactionStruct (136 bytes per record)
 *   - ticks.bin -> MDStockStruct (2216 bytes per record)
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <getopt.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "market_data_structs_aligned.h"

// ============================================================================
// Constants
// ============================================================================
constexpr size_t HEADER_SIZE = 64;

// Mmap file header structure
struct MmapHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t struct_size;
    uint64_t record_count;
    uint64_t write_offset;
    char reserved[40];
};
static_assert(sizeof(MmapHeader) == HEADER_SIZE, "Header size mismatch");

// Record types
enum class RecordType { ORDERS, TRANSACTIONS, TICKS, UNKNOWN };

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * Extract a null-terminated string from a fixed-size char array.
 * Filters non-ASCII characters to handle uninitialized data.
 */
static inline std::string extract_string(const char* data, size_t max_len) {
    std::string result;
    result.reserve(max_len);
    for (size_t i = 0; i < max_len && data[i] != '\0'; ++i) {
        char c = data[i];
        // Only keep printable ASCII characters
        if (c >= 32 && c <= 126) {
            // Escape special TSV characters
            if (c == '\t') {
                result += "\\t";
            } else if (c == '\n') {
                result += "\\n";
            } else if (c == '\\') {
                result += "\\\\";
            } else {
                result += c;
            }
        }
    }
    return result;
}

/**
 * Format an int64 array as ClickHouse array literal: [v1,v2,v3,...]
 */
template <size_t N>
static inline void format_int64_array(char* buf, size_t buf_size, const int64_t (&arr)[N]) {
    char* p = buf;
    char* end = buf + buf_size - 1;
    *p++ = '[';
    for (size_t i = 0; i < N && p < end; ++i) {
        if (i > 0) *p++ = ',';
        int written = snprintf(p, end - p, "%ld", arr[i]);
        if (written > 0) p += written;
    }
    if (p < end) *p++ = ']';
    *p = '\0';
}

// ============================================================================
// TSV Formatting Functions
// ============================================================================

/**
 * Format MDOrderStruct as TSV line.
 * Column order matches Python script's parse_order_v2():
 *   local_recv_timestamp, orderindex, orderprice, orderqty, orderno, tradedqty, applseqnum,
 *   mddate, mdtime, securityidsource, securitytype, ordertype, orderbsflag, channelno, datamultiplepowerof10,
 *   htscsecurityid, securitystatus
 */
static void format_order(const MDOrderStruct* rec, std::string& output) {
    char buf[512];
    int len = snprintf(buf, sizeof(buf),
        "%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t"
        "%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t",
        rec->local_recv_timestamp,
        rec->orderindex,
        rec->orderprice,
        rec->orderqty,
        rec->orderno,
        rec->tradedqty,
        rec->applseqnum,
        rec->mddate,
        rec->mdtime,
        rec->securityidsource,
        rec->securitytype,
        rec->ordertype,
        rec->orderbsflag,
        rec->channelno,
        rec->datamultiplepowerof10);

    output.append(buf, len);
    output.append(extract_string(rec->htscsecurityid, sizeof(rec->htscsecurityid)));
    output.push_back('\t');
    output.append(extract_string(rec->securitystatus, sizeof(rec->securitystatus)));
    output.push_back('\n');
}

/**
 * Format MDTransactionStruct as TSV line.
 * Column order matches Python script's parse_transaction_v2():
 *   local_recv_timestamp, tradeindex, tradebuyno, tradesellno, tradeprice, tradeqty, trademoney, applseqnum,
 *   mddate, mdtime, securityidsource, securitytype, tradetype, tradebsflag, channelno, datamultiplepowerof10,
 *   htscsecurityid
 */
static void format_transaction(const MDTransactionStruct* rec, std::string& output) {
    char buf[512];
    int len = snprintf(buf, sizeof(buf),
        "%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t"
        "%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t",
        rec->local_recv_timestamp,
        rec->tradeindex,
        rec->tradebuyno,
        rec->tradesellno,
        rec->tradeprice,
        rec->tradeqty,
        rec->trademoney,
        rec->applseqnum,
        rec->mddate,
        rec->mdtime,
        rec->securityidsource,
        rec->securitytype,
        rec->tradetype,
        rec->tradebsflag,
        rec->channelno,
        rec->datamultiplepowerof10);

    output.append(buf, len);
    output.append(extract_string(rec->htscsecurityid, sizeof(rec->htscsecurityid)));
    output.push_back('\n');
}

/**
 * Format MDStockStruct as TSV line.
 * Column order matches Python script's parse_tick_v2():
 *   (25 int64 scalars) local_recv_timestamp ... totalsellnumber,
 *   (8 int64 arrays) buypricequeue, buyorderqtyqueue, sellpricequeue, sellorderqtyqueue,
 *                    buyorderqueue, sellorderqueue, buynumordersqueue, sellnumordersqueue,
 *   (12 int32 scalars) mddate ... sellnumordersqueue_count,
 *   htscsecurityid, tradingphasecode
 */
static void format_tick(const MDStockStruct* rec, std::string& output) {
    // Buffer for scalar fields
    char buf[1024];
    int len = snprintf(buf, sizeof(buf),
        // 25 int64 scalars
        "%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t"
        "%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t"
        "%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t"
        "%ld\t%ld\t",
        rec->local_recv_timestamp,
        rec->datatimestamp,
        rec->maxpx,
        rec->minpx,
        rec->preclosepx,
        rec->numtrades,
        rec->totalvolumetrade,
        rec->totalvaluetrade,
        rec->lastpx,
        rec->openpx,
        rec->closepx,
        rec->highpx,
        rec->lowpx,
        rec->totalbuyqty,
        rec->totalsellqty,
        rec->weightedavgbuypx,
        rec->weightedavgsellpx,
        rec->withdrawbuynumber,
        rec->withdrawbuyamount,
        rec->withdrawbuymoney,
        rec->withdrawsellnumber,
        rec->withdrawsellamount,
        rec->withdrawsellmoney,
        rec->totalbuynumber,
        rec->totalsellnumber);
    output.append(buf, len);

    // 8 int64 arrays (4x10 + 4x50)
    char arr_buf[1024];

    format_int64_array(arr_buf, sizeof(arr_buf), rec->buypricequeue);
    output.append(arr_buf);
    output.push_back('\t');

    format_int64_array(arr_buf, sizeof(arr_buf), rec->buyorderqtyqueue);
    output.append(arr_buf);
    output.push_back('\t');

    format_int64_array(arr_buf, sizeof(arr_buf), rec->sellpricequeue);
    output.append(arr_buf);
    output.push_back('\t');

    format_int64_array(arr_buf, sizeof(arr_buf), rec->sellorderqtyqueue);
    output.append(arr_buf);
    output.push_back('\t');

    format_int64_array(arr_buf, sizeof(arr_buf), rec->buyorderqueue);
    output.append(arr_buf);
    output.push_back('\t');

    format_int64_array(arr_buf, sizeof(arr_buf), rec->sellorderqueue);
    output.append(arr_buf);
    output.push_back('\t');

    format_int64_array(arr_buf, sizeof(arr_buf), rec->buynumordersqueue);
    output.append(arr_buf);
    output.push_back('\t');

    format_int64_array(arr_buf, sizeof(arr_buf), rec->sellnumordersqueue);
    output.append(arr_buf);
    output.push_back('\t');

    // 12 int32 scalars
    len = snprintf(buf, sizeof(buf),
        "%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t"
        "%d\t%d\t%d\t%d\t",
        rec->mddate,
        rec->mdtime,
        rec->securityidsource,
        rec->securitytype,
        rec->numbuyorders,
        rec->numsellorders,
        rec->channelno,
        rec->datamultiplepowerof10,
        rec->buyorderqueue_count,
        rec->sellorderqueue_count,
        rec->buynumordersqueue_count,
        rec->sellnumordersqueue_count);
    output.append(buf, len);

    // htscsecurityid
    output.append(extract_string(rec->htscsecurityid, sizeof(rec->htscsecurityid)));
    output.push_back('\t');

    // tradingphasecode (single char)
    if (rec->tradingphasecode >= 32 && rec->tradingphasecode <= 126) {
        output.push_back(rec->tradingphasecode);
    }
    output.push_back('\n');
}

// ============================================================================
// Parallel Processing
// ============================================================================

/**
 * Process a chunk of records in a worker thread.
 */
template <typename T, void (*FormatFunc)(const T*, std::string&)>
static void process_chunk(const char* base, size_t start, size_t end,
                          size_t record_size, std::string& output) {
    // Estimate ~300 bytes per line for reservation
    output.reserve((end - start) * 300);

    for (size_t i = start; i < end; ++i) {
        const char* record_ptr = base + HEADER_SIZE + i * record_size;
        const T* rec = reinterpret_cast<const T*>(record_ptr);
        FormatFunc(rec, output);
    }
}

/**
 * Process all records using multiple threads.
 */
template <typename T, void (*FormatFunc)(const T*, std::string&)>
static void process_parallel(const char* base, size_t record_count, size_t record_size, int num_threads) {
    if (record_count == 0) return;

    // Adjust thread count based on record count
    size_t actual_threads = std::min(static_cast<size_t>(num_threads), record_count);

    std::vector<std::thread> threads;
    std::vector<std::string> outputs(actual_threads);

    size_t chunk_size = record_count / actual_threads;
    size_t remainder = record_count % actual_threads;

    size_t start = 0;
    for (size_t t = 0; t < actual_threads; ++t) {
        size_t end = start + chunk_size + (t < remainder ? 1 : 0);
        threads.emplace_back(process_chunk<T, FormatFunc>, base, start, end, record_size, std::ref(outputs[t]));
        start = end;
    }

    // Wait for all threads and output in order
    for (size_t t = 0; t < actual_threads; ++t) {
        threads[t].join();
        fwrite(outputs[t].data(), 1, outputs[t].size(), stdout);
    }
}

// ============================================================================
// Main
// ============================================================================

static void print_usage(const char* prog) {
    fprintf(stderr,
        "Usage: %s [options] <bin_file>\n"
        "\n"
        "Export mmap binary file to TSV format (stdout) for ClickHouse import.\n"
        "\n"
        "Options:\n"
        "  --type TYPE      Record type: orders, transactions, or ticks\n"
        "                   (auto-detected from magic if not specified)\n"
        "  --threads N      Number of threads (default: 16)\n"
        "  --limit N        Limit number of records (for testing)\n"
        "  -h, --help       Show this help message\n"
        "\n"
        "Examples:\n"
        "  %s --type orders /path/to/orders.bin | \\\n"
        "      clickhouse-client --query \"INSERT INTO MDOrderStruct FORMAT TabSeparated\"\n"
        "\n"
        "  %s --type transactions --limit 1000 /path/to/transactions.bin\n"
        "\n",
        prog, prog, prog);
}

int main(int argc, char** argv) {
    // Parse command line arguments
    const char* type_str = nullptr;
    int num_threads = 16;
    size_t limit = 0;
    const char* filepath = nullptr;

    static struct option long_options[] = {
        {"type", required_argument, nullptr, 't'},
        {"threads", required_argument, nullptr, 'n'},
        {"limit", required_argument, nullptr, 'l'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "t:n:l:h", long_options, nullptr)) != -1) {
        switch (opt) {
            case 't':
                type_str = optarg;
                break;
            case 'n':
                num_threads = atoi(optarg);
                if (num_threads < 1) num_threads = 1;
                if (num_threads > 64) num_threads = 64;
                break;
            case 'l':
                limit = strtoul(optarg, nullptr, 10);
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "Error: No input file specified\n\n");
        print_usage(argv[0]);
        return 1;
    }
    filepath = argv[optind];

    // Open and mmap the file
    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        close(fd);
        return 1;
    }

    size_t file_size = st.st_size;
    if (file_size < HEADER_SIZE) {
        fprintf(stderr, "Error: File too small (size=%zu, need at least %zu)\n",
                file_size, HEADER_SIZE);
        close(fd);
        return 1;
    }

    void* mapped = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }

    const char* base = static_cast<const char*>(mapped);
    const MmapHeader* header = reinterpret_cast<const MmapHeader*>(base);

    // Detect record type from magic or command line
    RecordType record_type = RecordType::UNKNOWN;
    size_t record_size = 0;

    if (type_str != nullptr) {
        if (strcmp(type_str, "orders") == 0) {
            record_type = RecordType::ORDERS;
            record_size = sizeof(MDOrderStruct);
        } else if (strcmp(type_str, "transactions") == 0) {
            record_type = RecordType::TRANSACTIONS;
            record_size = sizeof(MDTransactionStruct);
        } else if (strcmp(type_str, "ticks") == 0) {
            record_type = RecordType::TICKS;
            record_size = sizeof(MDStockStruct);
        } else {
            fprintf(stderr, "Error: Unknown type '%s'. Use: orders, transactions, or ticks\n", type_str);
            munmap(mapped, file_size);
            close(fd);
            return 1;
        }
    } else {
        // Auto-detect from magic
        switch (header->magic) {
            case MAGIC_ORDER_V2:
                record_type = RecordType::ORDERS;
                record_size = sizeof(MDOrderStruct);
                break;
            case MAGIC_TRANSACTION_V2:
                record_type = RecordType::TRANSACTIONS;
                record_size = sizeof(MDTransactionStruct);
                break;
            case MAGIC_TICK_V2:
                record_type = RecordType::TICKS;
                record_size = sizeof(MDStockStruct);
                break;
            default:
                fprintf(stderr, "Error: Unknown magic 0x%08X. Use --type to specify record type.\n",
                        header->magic);
                munmap(mapped, file_size);
                close(fd);
                return 1;
        }
    }

    // Validate magic matches expected type
    uint32_t expected_magic = 0;
    const char* type_name = nullptr;
    switch (record_type) {
        case RecordType::ORDERS:
            expected_magic = MAGIC_ORDER_V2;
            type_name = "orders";
            break;
        case RecordType::TRANSACTIONS:
            expected_magic = MAGIC_TRANSACTION_V2;
            type_name = "transactions";
            break;
        case RecordType::TICKS:
            expected_magic = MAGIC_TICK_V2;
            type_name = "ticks";
            break;
        default:
            break;
    }

    if (header->magic != expected_magic) {
        fprintf(stderr, "Warning: Magic mismatch (file=0x%08X, expected=0x%08X for type=%s)\n",
                header->magic, expected_magic, type_name);
    }

    size_t record_count = header->record_count;
    if (limit > 0 && limit < record_count) {
        record_count = limit;
    }

    // Log to stderr
    fprintf(stderr, "File: %s\n", filepath);
    fprintf(stderr, "Type: %s (magic=0x%08X)\n", type_name, header->magic);
    fprintf(stderr, "Records: %zu (limit=%zu)\n", record_count, limit);
    fprintf(stderr, "Threads: %d\n", num_threads);

    // Process records
    switch (record_type) {
        case RecordType::ORDERS:
            process_parallel<MDOrderStruct, format_order>(base, record_count, record_size, num_threads);
            break;
        case RecordType::TRANSACTIONS:
            process_parallel<MDTransactionStruct, format_transaction>(base, record_count, record_size, num_threads);
            break;
        case RecordType::TICKS:
            process_parallel<MDStockStruct, format_tick>(base, record_count, record_size, num_threads);
            break;
        default:
            break;
    }

    // Cleanup
    munmap(mapped, file_size);
    close(fd);

    fprintf(stderr, "Done: %zu records exported\n", record_count);
    return 0;
}
