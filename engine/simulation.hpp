#pragma once
#include "matching_engine.hpp"
#include "metrics.hpp"
#include <string>

class Simulation {
public:
    // CSV format: timestamp,symbol,price,quantity,side,trader_id[,type[,peak_size]]
    void replayCSV(const std::string& csv_path);
    void printMetrics()                           const;
    void exportMetrics(const std::string& path)   const;

private:
    MatchingEngine engine_;
    Metrics        metrics_;

    struct CsvRow {
        uint64_t  timestamp_ns;
        char      symbol[8];
        double    price;
        int32_t   quantity;
        Side      side;
        uint64_t  trader_id;
        OrderType type;
        int32_t   peak_size;
    };

    CsvRow parseRow(const std::string& line, int line_num);
};
