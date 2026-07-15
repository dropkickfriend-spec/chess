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

-- Initialize current weights with defaults
INSERT INTO strategy_weights_current (strategy_idx, strategy_name, weight)
VALUES
  (0, 'MATERIAL', 1.0),
  (1, 'DEVELOPMENT', 1.0),
  (2, 'CENTER_CONTROL', 1.0),
  (3, 'KING_SAFETY_OPENING', 1.0),
  (4, 'PIECE_ACTIVITY', 1.0),
  (5, 'ATTACK_POTENTIAL', 1.0),
  (6, 'PAWN_STRUCTURE', 1.0),
  (7, 'DEFENDER_LOGISTICS', 1.0),
  (8, 'KING_ACTIVITY', 1.0),
  (9, 'PAWN_PROMOTION', 1.0),
  (10, 'OPPOSITION', 1.0)
ON CONFLICT (strategy_idx) DO NOTHING;

CREATE INDEX idx_strategy_weights_game_id ON strategy_weights(game_id);
CREATE INDEX idx_strategy_weights_timestamp ON strategy_weights(timestamp);
CREATE INDEX idx_strategy_weights_strategy_idx ON strategy_weights(strategy_idx);
