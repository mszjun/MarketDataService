-- Tick (depth market data) storage for the MarketDataService.
--
-- One row per CTP OnRtnDepthMarketData callback.
-- Run against your ClickHouse server before the first insert, e.g.
--   clickhouse-client --query "CREATE DATABASE IF NOT EXISTS market"
--   clickhouse-client --multiquery < create_tick_table.sql

CREATE TABLE IF NOT EXISTS market.tick
(
    trading_day           Date,                       -- CTP TradingDay
    exchange              LowCardinality(String),     -- canonical: SHFE/DCE/ZCE/CFFE/INE/GFEX
    instrument            String,                     -- CTP InstrumentID, e.g. rb2405
    update_dt             DateTime64(3, 'Asia/Shanghai'), -- ActionDay + UpdateTime + UpdateMillisec (Beijing)
    seq                   UInt64,                     -- per-process sequence (informational)

    pre_settlement_price  Float64,                    -- PreSettlementPrice (0 when absent)
    pre_close_price       Float64,                    -- PreClosePrice
    pre_open_interest     Float64,                    -- PreOpenInterest
    latest_price          Float64,                    -- LastPrice
    open_price            Float64,                    -- OpenPrice
    highest_price         Float64,                    -- HighestPrice
    lowest_price          Float64,                    -- LowestPrice
    volume                UInt64,                     -- Volume
    turnover              Float64,                    -- Turnover
    open_interest         Float64,                    -- OpenInterest
    close_price           Float64,                    -- ClosePrice
    settlement_price      Float64,                    -- SettlementPrice
    upper_limit_price     Float64,                    -- UpperLimitPrice
    lower_limit_price     Float64,                    -- LowerLimitPrice

    bid_price_1           Float64,
    bid_volume_1          UInt64,
    ask_price_1           Float64,
    ask_volume_1          UInt64
)
ENGINE = MergeTree
PARTITION BY toYYYYMM(trading_day)
ORDER BY (exchange, instrument, update_dt)
SETTINGS index_granularity = 8192;

-- convenience views / examples -------------------------------------------------

-- latest tick per instrument (per trading day partition)
-- SELECT *
-- FROM market.tick
-- WHERE trading_day = today()
-- ORDER BY instrument, update_dt DESC
-- LIMIT 1 BY instrument;
