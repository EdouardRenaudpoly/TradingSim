#include "simulation.hpp"
#include "trade_store.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <stdexcept>

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
}

static Side parseSide(const std::string& s) {
    return (s == "BUY") ? Side::BUY : Side::SELL;
}

static OrderType parseType(const std::string& s) {
    if (s == "MARKET")    return OrderType::MARKET;
    if (s == "IOC")       return OrderType::IOC;
    if (s == "FOK")       return OrderType::FOK;
    if (s == "POST_ONLY") return OrderType::POST_ONLY;
    if (s == "ICEBERG")   return OrderType::ICEBERG;
    return OrderType::LIMIT;
}

Simulation::CsvRow Simulation::parseRow(const std::string& line, int line_num) {
    // CSV: timestamp,symbol,price,quantity,side,trader_id[,type[,peak_size]]
    std::istringstream ss(line);
    std::string tok;
    std::vector<std::string> f;
    while (std::getline(ss, tok, ','))
        f.push_back(trim(tok));

    if (f.size() < 6)
        throw std::runtime_error("line " + std::to_string(line_num) +
                                 ": expected at least 6 fields");

    CsvRow row{};
    row.timestamp_ns = std::stoull(f[0]);
    std::strncpy(row.symbol, f[1].c_str(), 7);
    row.symbol[7] = '\0';
    row.price     = std::stod(f[2]);
    row.quantity  = std::stoi(f[3]);
    row.side      = parseSide(f[4]);
    row.trader_id = std::stoull(f[5]);
    row.type      = (f.size() >= 7) ? parseType(f[6]) : OrderType::LIMIT;
    row.peak_size = (f.size() >= 8) ? std::stoi(f[7]) : 0;
    return row;
}

void Simulation::replayCSV(const std::string& csv_path) {
    std::ifstream f(csv_path);
    if (!f.is_open())
        throw std::runtime_error("Cannot open: " + csv_path);

    std::string line;
    std::getline(f, line); // skip header
    int line_num = 1;

    while (std::getline(f, line)) {
        ++line_num;
        if (trim(line).empty()) continue;

        try {
            CsvRow row = parseRow(line, line_num);

            engine_.submit(row.trader_id, row.symbol,
                           row.price, row.quantity,
                           row.side, row.type, row.peak_size);

            auto trades = engine_.processAll();
            for (const auto& t : trades) {
                metrics_.record(t);
                all_trades_.push_back(t);
            }

        } catch (const std::exception& e) {
            std::cerr << "[warn] " << e.what() << "\n";
        }
    }
}

void Simulation::printMetrics() const {
    metrics_.print();
    engine_.stats().print();
    engine_.latency().print();
}

void Simulation::exportMetrics(const std::string& path) const {
    metrics_.exportCSV(path);
}

void Simulation::exportDB(const std::string& db_path) const {
    TradeStore store(db_path);
    store.insertTrades(all_trades_);
    store.insertMetrics(metrics_);
}
