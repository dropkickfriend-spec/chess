-- Strategy weights table for self-tuning evaluation
CREATE TABLE IF NOT EXISTS strategy_weights (
  id BIGSERIAL PRIMARY KEY,
  game_id BIGINT,
  timestamp TIMESTAMPTZ DEFAULT NOW(),
  strategy_idx INT NOT NULL,
  strategy_name TEXT NOT NULL,
  weight FLOAT NOT NULL,
  CONSTRAINT strategy_idx_valid CHECK (strategy_idx >= 0 AND strategy_idx < 11)
);

-- Current weights snapshot (latest version of each strategy)
CREATE TABLE IF NOT EXISTS strategy_weights_current (
  strategy_idx INT PRIMARY KEY,
  strategy_name TEXT NOT NULL,
  weight FLOAT NOT NULL,
  last_updated TIMESTAMPTZ DEFAULT NOW()
);

-- Initialize current weights with defaults.
-- Index order MUST match the StrategyType enum in src/eval_strategy.h
-- (and STRATEGIES in tools/analyze_game.py).
INSERT INTO strategy_weights_current (strategy_idx, strategy_name, weight)
VALUES
  (0,  'DEVELOPMENT', 1.0),
  (1,  'CENTER_CONTROL', 1.0),
  (2,  'KING_SAFETY_OPENING', 1.0),
  (3,  'PIECE_ACTIVITY', 1.0),
  (4,  'ATTACK_POTENTIAL', 1.0),
  (5,  'PAWN_STRUCTURE', 1.0),
  (6,  'DEFENDER_LOGISTICS', 1.0),
  (7,  'KING_ACTIVITY', 1.0),
  (8,  'PAWN_PROMOTION', 1.0),
  (9,  'OPPOSITION', 1.0),
  (10, 'MATERIAL', 1.0)
ON CONFLICT (strategy_idx) DO NOTHING;

CREATE INDEX IF NOT EXISTS idx_strategy_weights_game_id ON strategy_weights(game_id);
CREATE INDEX IF NOT EXISTS idx_strategy_weights_timestamp ON strategy_weights(timestamp);
CREATE INDEX IF NOT EXISTS idx_strategy_weights_strategy_idx ON strategy_weights(strategy_idx);

-- Same access model as the chessbb tables: anon may append history and
-- upsert the current snapshot; everyone may read.
ALTER TABLE strategy_weights ENABLE ROW LEVEL SECURITY;
ALTER TABLE strategy_weights_current ENABLE ROW LEVEL SECURITY;

CREATE POLICY strategy_weights_insert ON strategy_weights
  FOR INSERT TO anon WITH CHECK (true);
CREATE POLICY strategy_weights_select ON strategy_weights
  FOR SELECT TO anon, authenticated USING (true);

CREATE POLICY strategy_weights_current_insert ON strategy_weights_current
  FOR INSERT TO anon WITH CHECK (true);
CREATE POLICY strategy_weights_current_update ON strategy_weights_current
  FOR UPDATE TO anon USING (true) WITH CHECK (true);
CREATE POLICY strategy_weights_current_select ON strategy_weights_current
  FOR SELECT TO anon, authenticated USING (true);
