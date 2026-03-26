//=================================================================
// MNQ-MES-ES-NQ EMA21 FLIP STRATEGY BACKTESTER  v1.0
// C++ — mirrors Pine Script logic exactly
//
// Strategy : EMA21 cross → long/short flip
// TP       : +55 price pts   SL: -30 price pts
// Delay    : 3-second re-entry block after any exit
// Scaling  : +10 micros / +1 mini per 15% equity gain
// Capital  : $150,000 starting
//
// Build  :  g++ -O2 -std=c++17 -o backtester ema21_flip_backtest.cpp
// Run    :  ./backtester [data_directory]
//
// ─── CSV format (with header row) ─────────────────────────────
//   timestamp,open,high,low,close,volume
//   timestamp = Unix milliseconds
//
// ─── File naming ──────────────────────────────────────────────
//   {SYMBOL}_{TIMEFRAME}.csv
//   Examples:  MES_5sec.csv   MNQ_15sec.csv
//              ES_30sec.csv   NQ_1min.csv
//=================================================================

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <map>
#include <numeric>
#include <climits>
#include <cassert>

// ─────────────────────────────────────────────────────────────
//  Data Structures
// ─────────────────────────────────────────────────────────────

struct Bar {
    long long timestamp;              // Unix ms
    double    open, high, low, close, volume;
};

struct Trade {
    long long   entryTime, exitTime;
    int         direction;            // +1 long  |  -1 short
    double      entryPrice, exitPrice;
    int         qty;
    double      pnl;
    std::string exitReason;           // TP | SL | SIGNAL | FORCE
};

struct Config {
    std::string name;
    double      contractMultiplier;   // dollars per price point
    bool        isMicro;              // true = micro, false = mini
    int         baseQty;              // starting contract count
    int         qtyStep;              // contracts added per tier
    double      startEquity;          // initial capital
    double      scalePct;             // equity growth % per tier (0.15 = 15%)
    int         emaPeriod;
    double      tpOffset;             // price-point TP offset
    double      slOffset;             // price-point SL offset
    long long   delayMs;              // min ms between trades
};

struct Stats {
    // Overall
    double finalEquity;
    double totalPnL;
    double totalPnLPct;
    double maxDrawdown;
    double maxDrawdownPct;
    int    maxDDAtTrade;              // trade# when max DD was reached
    int    totalTrades;
    int    profitableTrades;
    double winRate;
    double profitFactor;
    double grossProfit;
    double grossLoss;
    double avgWin;
    double avgLoss;
    double expectancy;
    int    tpCount, slCount, sigCount;
    // Last 90 days
    int    trades90d;
    int    profitable90d;
    double winRate90d;
    double pf90d;
    double pnl90d;
    double grossProfit90d;
    double grossLoss90d;
    // Trade log
    std::vector<Trade> trades;
};

// ─────────────────────────────────────────────────────────────
//  CSV Loader
//  Expects: timestamp,open,high,low,close,volume  (header row)
//  Handles both comma and tab delimiters
// ─────────────────────────────────────────────────────────────

std::vector<Bar> loadCSV(const std::string& path) {
    std::vector<Bar> bars;
    std::ifstream f(path);
    if (!f.is_open()) return bars;

    std::string line;
    std::getline(f, line); // skip header

    while (std::getline(f, line)) {
        if (line.empty()) continue;
        // Accept comma or tab delimiter
        char delim = (line.find('\t') != std::string::npos) ? '\t' : ',';
        std::istringstream ss(line);
        std::string tok;
        Bar b{};
        int col = 0;
        while (std::getline(ss, tok, delim)) {
            try {
                switch (col++) {
                    case 0: b.timestamp = std::stoll(tok); break;
                    case 1: b.open      = std::stod(tok);  break;
                    case 2: b.high      = std::stod(tok);  break;
                    case 3: b.low       = std::stod(tok);  break;
                    case 4: b.close     = std::stod(tok);  break;
                    case 5: b.volume    = std::stod(tok);  break;
                }
            } catch (...) {}
        }
        if (col >= 5 && b.high >= b.low && b.close > 0)
            bars.push_back(b);
    }
    return bars;
}

// ─────────────────────────────────────────────────────────────
//  EMA Calculator  (Pine Script compatible: SMA seed, then EMA)
// ─────────────────────────────────────────────────────────────

std::vector<double> calcEMA(const std::vector<Bar>& bars, int period) {
    std::vector<double> ema(bars.size(), 0.0);
    if ((int)bars.size() < period) return ema;

    // Seed with SMA of first `period` closes
    double sum = 0.0;
    for (int i = 0; i < period; i++) sum += bars[i].close;
    ema[period - 1] = sum / period;

    const double mult = 2.0 / (period + 1.0);
    for (size_t i = period; i < bars.size(); i++)
        ema[i] = bars[i].close * mult + ema[i-1] * (1.0 - mult);

    return ema;
}

// ─────────────────────────────────────────────────────────────
//  Position Size Calculator  (compound 15% tiers)
// ─────────────────────────────────────────────────────────────

int getQty(double equity, const Config& cfg) {
    int    qty       = cfg.baseQty;
    double threshold = cfg.startEquity * (1.0 + cfg.scalePct);
    while (equity >= threshold) {
        qty       += cfg.qtyStep;
        threshold *= (1.0 + cfg.scalePct);
    }
    return qty;
}

// ─────────────────────────────────────────────────────────────
//  Core Backtest Engine
// ─────────────────────────────────────────────────────────────

Stats runBacktest(const std::vector<Bar>& bars, const Config& cfg) {
    Stats s{};
    if ((int)bars.size() < cfg.emaPeriod + 1) return s;

    auto ema = calcEMA(bars, cfg.emaPeriod);

    double equity  = cfg.startEquity;
    double peakEq  = equity;
    double maxDD   = 0.0;
    int    ddAt    = 0;

    // Position state
    int       posDir   = 0;
    double    posPrice = 0.0;
    int       posQty   = 0;
    long long posTime  = 0;
    long long lastExit = LLONG_MIN;

    std::vector<Trade>& trades = s.trades;

    // Lambdas ─────────────────────────────────────────────────

    auto updateDD = [&]() {
        if (equity > peakEq) peakEq = equity;
        double dd = peakEq - equity;
        if (dd > maxDD) { maxDD = dd; ddAt = (int)trades.size(); }
    };

    auto doClose = [&](long long ts, double price, const std::string& reason) {
        double pnl = posDir * (price - posPrice) * posQty * cfg.contractMultiplier;
        equity += pnl;

        Trade t;
        t.entryTime  = posTime;
        t.exitTime   = ts;
        t.direction  = posDir;
        t.entryPrice = posPrice;
        t.exitPrice  = price;
        t.qty        = posQty;
        t.pnl        = pnl;
        t.exitReason = reason;
        trades.push_back(t);

        lastExit = ts;
        posDir   = 0;
        updateDD();
    };

    // Main loop ───────────────────────────────────────────────

    for (size_t i = (size_t)cfg.emaPeriod; i < bars.size(); i++) {
        const Bar& bar  = bars[i];
        const Bar& prev = bars[i-1];

        // Pine Script ta.crossover / ta.crossunder:
        // crossover(close, ema)  = prev.close < prev.ema  AND cur.close > cur.ema
        // crossunder(close, ema) = prev.close > prev.ema  AND cur.close < cur.ema
        bool buySignal  = (prev.close < ema[i-1]) && (bar.close > ema[i]);
        bool sellSignal = (prev.close > ema[i-1]) && (bar.close < ema[i]);

        // ── canEnter evaluated BEFORE any exits (Pine Script order) ──
        // This allows same-bar reversal if delay already satisfied
        bool canEnter = (lastExit == LLONG_MIN) ||
                        ((bar.timestamp - lastExit) > cfg.delayMs);

        // ── TP / SL  (intrabar, using high/low) ──────────────────────
        if (posDir != 0) {
            double tpPrice, slPrice;
            bool   tpHit = false, slHit = false;

            if (posDir == 1) {               // Long
                tpPrice = posPrice + cfg.tpOffset;
                slPrice = posPrice - cfg.slOffset;
                tpHit   = (bar.high  >= tpPrice);
                slHit   = (bar.low   <= slPrice);
            } else {                         // Short
                tpPrice = posPrice - cfg.tpOffset;
                slPrice = posPrice + cfg.slOffset;
                tpHit   = (bar.low   <= tpPrice);
                slHit   = (bar.high  >= slPrice);
            }

            if (tpHit || slHit) {
                // Both hit same bar → open proximity decides which came first
                bool slFirst = false;
                if (tpHit && slHit) {
                    if (posDir == 1)
                        slFirst = (bar.open <= slPrice) ||
                                  ((bar.open - slPrice) < (tpPrice - bar.open));
                    else
                        slFirst = (bar.open >= slPrice) ||
                                  ((slPrice - bar.open) < (bar.open - tpPrice));
                }

                if ((slHit && slFirst) || (slHit && !tpHit)) {
                    doClose(bar.timestamp, slPrice, "SL");
                    s.slCount++;
                } else {
                    doClose(bar.timestamp, tpPrice, "TP");
                    s.tpCount++;
                }

                // After TP/SL intrabar → block same-bar signal entry
                canEnter = false;
            }
        }

        // ── Signal-based reversal closes ─────────────────────────────
        if (posDir == 1 && sellSignal) {
            doClose(bar.timestamp, bar.close, "SIGNAL");
            s.sigCount++;
        }
        if (posDir == -1 && buySignal) {
            doClose(bar.timestamp, bar.close, "SIGNAL");
            s.sigCount++;
        }

        // ── New entries ───────────────────────────────────────────────
        if (posDir == 0 && canEnter) {
            int qty = getQty(equity, cfg);
            if (buySignal) {
                posDir = 1;  posPrice = bar.close;
                posQty = qty; posTime = bar.timestamp;
            } else if (sellSignal) {
                posDir = -1; posPrice = bar.close;
                posQty = qty; posTime = bar.timestamp;
            }
        }
    }

    // Force-close any open position at last bar
    if (posDir != 0) {
        doClose(bars.back().timestamp, bars.back().close, "FORCE");
        s.sigCount++;
    }

    // ── Statistics ────────────────────────────────────────────────────

    s.finalEquity    = equity;
    s.totalPnL       = equity - cfg.startEquity;
    s.totalPnLPct    = s.totalPnL / cfg.startEquity * 100.0;
    s.maxDrawdown    = maxDD;
    s.maxDrawdownPct = (peakEq > 0) ? maxDD / peakEq * 100.0 : 0.0;
    s.maxDDAtTrade   = ddAt;
    s.totalTrades    = (int)trades.size();

    // 90-day cutoff from last bar
    long long latestTs = bars.back().timestamp;
    long long cut90    = latestTs - (long long)90 * 86400LL * 1000LL;

    double gp = 0, gl = 0, gp90 = 0, gl90 = 0;
    int wins = 0, wins90 = 0, cnt90 = 0;

    for (const auto& t : trades) {
        if (t.pnl > 0) { gp += t.pnl; wins++; }
        else            { gl -= t.pnl;         }

        if (t.exitTime >= cut90) {
            cnt90++;
            if (t.pnl > 0) { gp90 += t.pnl; wins90++; }
            else            { gl90 -= t.pnl;           }
        }
    }

    s.profitableTrades = wins;
    s.grossProfit      = gp;
    s.grossLoss        = gl;
    s.winRate          = s.totalTrades > 0 ? 100.0 * wins / s.totalTrades : 0;
    s.profitFactor     = gl > 0 ? gp / gl : (gp > 0 ? 9999.0 : 0);
    s.avgWin           = wins > 0              ? gp / wins                      : 0;
    s.avgLoss          = (s.totalTrades-wins)>0? gl / (s.totalTrades-wins)      : 0;
    s.expectancy       = s.totalTrades > 0 ?
                         (s.winRate/100.0 * s.avgWin) -
                         ((1.0-s.winRate/100.0) * s.avgLoss) : 0;

    s.trades90d      = cnt90;
    s.profitable90d  = wins90;
    s.winRate90d     = cnt90 > 0 ? 100.0 * wins90 / cnt90 : 0;
    s.pf90d          = gl90 > 0 ? gp90 / gl90 : (gp90 > 0 ? 9999.0 : 0);
    s.pnl90d         = gp90 - gl90;
    s.grossProfit90d = gp90;
    s.grossLoss90d   = gl90;

    return s;
}

// ─────────────────────────────────────────────────────────────
//  Console Report
// ─────────────────────────────────────────────────────────────

static std::string dollar(double v, int w = 14) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(2) << std::setw(w) << v;
    return o.str();
}
static std::string pct(double v, int w = 10) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(2) << std::setw(w) << v << "%";
    return o.str();
}
static std::string num(double v, int w = 10, int d = 3) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(d) << std::setw(w) << v;
    return o.str();
}
static std::string intStr(int v, int w = 10) {
    std::ostringstream o;
    o << std::setw(w) << v;
    return o.str();
}

void printReport(const std::string& label, const Stats& s, const Config& cfg) {
    const std::string sep = "╠═══════════════════════════════════════════════════════════╣";
    const std::string top = "╔═══════════════════════════════════════════════════════════╗";
    const std::string bot = "╚═══════════════════════════════════════════════════════════╝";
    auto row = [](const std::string& k, const std::string& v) {
        // Fixed 61-char inner width
        std::string line = "║  " + k;
        line.resize(35, ' ');
        line += v;
        line.resize(62, ' ');
        line += "║";
        std::cout << line << "\n";
    };

    std::cout << "\n" << top << "\n";
    {
        std::string title = "  BACKTEST: " + label;
        std::string ln = "║" + title;
        ln.resize(62, ' ');
        ln += "║";
        std::cout << ln << "\n";
    }
    std::cout << sep << "\n";
    row("Starting Equity",  "$" + dollar(cfg.startEquity));
    row("Final Equity",     "$" + dollar(s.finalEquity));
    row("Total P&L",        "$" + dollar(s.totalPnL) + "  (" + num(s.totalPnLPct,7,2) + "%)");
    std::cout << sep << "\n";
    row("TRADE STATISTICS", "");
    std::cout << sep << "\n";
    row("Total Trades",     intStr(s.totalTrades));
    row("Profitable",       intStr(s.profitableTrades) + "  (" + pct(s.winRate,6) + ")");
    row("Profit Factor",    num(s.profitFactor,10,3));
    row("Gross Profit",     "$" + dollar(s.grossProfit));
    row("Gross Loss",       "$" + dollar(s.grossLoss));
    row("Avg Win",          "$" + dollar(s.avgWin));
    row("Avg Loss",         "$" + dollar(s.avgLoss));
    row("Expectancy/trade", "$" + dollar(s.expectancy));
    row("TP exits",         intStr(s.tpCount));
    row("SL exits",         intStr(s.slCount));
    row("Signal exits",     intStr(s.sigCount));
    std::cout << sep << "\n";
    row("RISK METRICS", "");
    std::cout << sep << "\n";
    row("Max Drawdown $",   "$" + dollar(s.maxDrawdown));
    row("Max Drawdown %",   pct(s.maxDrawdownPct, 8));
    row("Trade # at MaxDD", intStr(s.maxDDAtTrade) + "  of " + intStr(s.totalTrades));
    std::cout << sep << "\n";
    row("LAST 90 DAYS", "");
    std::cout << sep << "\n";
    row("Trades",           intStr(s.trades90d));
    row("Win Rate",         pct(s.winRate90d, 8));
    row("Profit Factor",    num(s.pf90d,10,3));
    row("P&L",              "$" + dollar(s.pnl90d));
    row("Gross Profit",     "$" + dollar(s.grossProfit90d));
    row("Gross Loss",       "$" + dollar(s.grossLoss90d));
    std::cout << bot << "\n";
}

// ─────────────────────────────────────────────────────────────
//  Export trade log to CSV
// ─────────────────────────────────────────────────────────────

void exportTrades(const std::vector<Trade>& trades,
                  const std::string& path, const Config& cfg) {
    std::ofstream f(path);
    if (!f.is_open()) return;
    f << "trade_num,entry_time_ms,exit_time_ms,direction,"
         "entry_price,exit_price,qty,pnl,exit_reason\n";
    for (int i = 0; i < (int)trades.size(); i++) {
        const auto& t = trades[i];
        f << (i+1) << ","
          << t.entryTime << ","
          << t.exitTime  << ","
          << (t.direction > 0 ? "LONG" : "SHORT") << ","
          << std::fixed << std::setprecision(4)
          << t.entryPrice << ","
          << t.exitPrice  << ","
          << t.qty        << ","
          << std::setprecision(2)
          << t.pnl        << ","
          << t.exitReason << "\n";
    }
}

// ─────────────────────────────────────────────────────────────
//  Export equity curve to CSV
// ─────────────────────────────────────────────────────────────

void exportEquityCurve(const std::vector<Trade>& trades,
                       const std::string& path,
                       double startEquity) {
    std::ofstream f(path);
    if (!f.is_open()) return;
    f << "trade_num,exit_time_ms,equity,pnl_cumulative\n";
    double equity = startEquity;
    f << "0,0," << std::fixed << std::setprecision(2) << equity << ",0.00\n";
    for (int i = 0; i < (int)trades.size(); i++) {
        equity += trades[i].pnl;
        f << (i+1)              << ","
          << trades[i].exitTime << ","
          << std::fixed << std::setprecision(2)
          << equity             << ","
          << (equity - startEquity) << "\n";
    }
}

// ─────────────────────────────────────────────────────────────
//  Multi-run comparison table
// ─────────────────────────────────────────────────────────────

void printComparisonTable(
    const std::vector<std::tuple<std::string,std::string,Stats>>& results)
{
    std::cout << "\n";
    std::cout << "╔══════════════╦══════════╦═══════════════╦══════════╦══════════╦══════════╦══════════╦══════════╗\n";
    std::cout << "║  Symbol      ║ Timeframe║   Total P&L   ║ Trades   ║ Win Rate ║  Pf(All) ║ Pf(90d)  ║ Max DD%  ║\n";
    std::cout << "╠══════════════╬══════════╬═══════════════╬══════════╬══════════╬══════════╬══════════╬══════════╣\n";

    for (const auto& [sym, tf, s] : results) {
        std::cout << "║ "
            << std::left  << std::setw(12) << sym    << " ║ "
            << std::left  << std::setw(8)  << tf     << " ║ $"
            << std::right << std::fixed << std::setprecision(0) << std::setw(12) << s.totalPnL << " ║ "
            << std::right << std::setw(8)  << s.totalTrades << " ║ "
            << std::right << std::fixed << std::setprecision(2) << std::setw(7) << s.winRate << "% ║ "
            << std::right << std::fixed << std::setprecision(2) << std::setw(8) << s.profitFactor << " ║ "
            << std::right << std::fixed << std::setprecision(2) << std::setw(8) << s.pf90d << " ║ "
            << std::right << std::fixed << std::setprecision(2) << std::setw(7) << s.maxDrawdownPct << "% ║\n";
    }
    std::cout << "╚══════════════╩══════════╩═══════════════╩══════════╩══════════╩══════════╩══════════╩══════════╝\n";
}

// ─────────────────────────────────────────────────────────────
//  Main
// ─────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {

    std::cout << "\n";
    std::cout << "╔═══════════════════════════════════════════════════════════╗\n";
    std::cout << "║      MNQ-MES-ES-NQ  EMA21 FLIP  BACKTESTER  v1.0        ║\n";
    std::cout << "║  Pine Script → C++  |  150K start  |  15% tier scale    ║\n";
    std::cout << "╚═══════════════════════════════════════════════════════════╝\n";

    // ─── Instrument Configurations ──────────────────────────────────
    //
    //  contractMultiplier = dollars per 1.0 price-point move per contract
    //    MES  = $5   per point  (Micro E-mini S&P 500)
    //    ES   = $50  per point  (E-mini S&P 500)
    //    MNQ  = $2   per point  (Micro E-mini Nasdaq-100)
    //    NQ   = $20  per point  (E-mini Nasdaq-100)
    //    MGC  = $10  per $/oz   (Micro Gold, 10 oz contract)
    //
    //  tpOffset / slOffset are the SAME price-unit offsets as Pine Script:
    //    TP = entry_price + 55   (profit target)
    //    SL = entry_price - 30   (stop loss)
    //
    //  Position scaling:
    //    Micro → start 10, add 10 per 15% equity tier
    //    Mini  → start  1, add  1 per 15% equity tier

    std::vector<Config> configs = {
    //   name    mult  micro  base step equity  scale  ema  tp    sl    delay
        {"MES",   5.0,  true,  10,  10, 150000, 0.15,  21,  55.0, 30.0, 3000},
        {"MNQ",   2.0,  true,  10,  10, 150000, 0.15,  21,  55.0, 30.0, 3000},
        {"ES",   50.0, false,   1,   1, 150000, 0.15,  21,  55.0, 30.0, 3000},
        {"NQ",   20.0, false,   1,   1, 150000, 0.15,  21,  55.0, 30.0, 3000},
        {"MGC",  10.0,  true,  10,  10, 150000, 0.15,  21,  55.0, 30.0, 3000},
    };

    // ─── Timeframes to test ──────────────────────────────────────────
    // Each entry: { label , csv_suffix }
    std::vector<std::pair<std::string,std::string>> timeframes = {
        {"5s",  "5sec"},
        {"15s", "15sec"},
        {"30s", "30sec"},
        {"1m",  "1min"},
    };

    std::string dataDir = "./data/";
    if (argc > 1) {
        dataDir = std::string(argv[1]);
        if (dataDir.back() != '/') dataDir += '/';
    }

    std::vector<std::tuple<std::string,std::string,Stats>> allResults;
    int ran = 0;

    for (const auto& cfg : configs) {
        for (const auto& [tfLabel, tfFile] : timeframes) {

            std::string csvPath = dataDir + cfg.name + "_" + tfFile + ".csv";
            std::vector<Bar> bars = loadCSV(csvPath);

            if (bars.empty()) {
                std::cout << "[SKIP]  " << csvPath << "  (not found or empty)\n";
                continue;
            }

            ran++;
            std::string label = cfg.name + " " + tfLabel;
            std::cout << "\n[LOAD]  " << csvPath
                      << "  —  " << bars.size() << " bars\n";

            Stats s = runBacktest(bars, cfg);
            printReport(label, s, cfg);

            // Save trade log
            std::string tradeOut  = dataDir + cfg.name + "_" + tfFile + "_trades.csv";
            std::string equityOut = dataDir + cfg.name + "_" + tfFile + "_equity.csv";
            exportTrades(s.trades, tradeOut, cfg);
            exportEquityCurve(s.trades, equityOut, cfg.startEquity);
            std::cout << "[OUT]   " << tradeOut << "\n";
            std::cout << "[OUT]   " << equityOut << "\n";

            allResults.emplace_back(cfg.name, tfLabel, s);
        }
    }

    // ─── Summary comparison table ────────────────────────────────────
    if (ran > 1) {
        std::cout << "\n\n=== COMPARISON SUMMARY ===";
        printComparisonTable(allResults);
    }

    // ─── Usage hint if no data found ─────────────────────────────────
    if (ran == 0) {
        std::cout << "\n";
        std::cout << "──────────────────────────────────────────────────────────────\n";
        std::cout << "  No CSV data found in: " << dataDir << "\n\n";
        std::cout << "  Required file naming:\n";
        std::cout << "    {SYMBOL}_{TIMEFRAME}.csv\n\n";
        std::cout << "  Examples:\n";
        std::cout << "    ./data/MES_5sec.csv\n";
        std::cout << "    ./data/MNQ_15sec.csv\n";
        std::cout << "    ./data/ES_30sec.csv\n";
        std::cout << "    ./data/NQ_1min.csv\n\n";
        std::cout << "  CSV format (header required):\n";
        std::cout << "    timestamp,open,high,low,close,volume\n";
        std::cout << "    (timestamp = Unix milliseconds)\n\n";
        std::cout << "  Pass data directory as argument:\n";
        std::cout << "    ./backtester /path/to/data\n";
        std::cout << "──────────────────────────────────────────────────────────────\n";
    }

    return 0;
}
