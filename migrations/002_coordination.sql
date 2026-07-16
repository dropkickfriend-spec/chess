-- COORDINATION joins as strategy 11; widen the index guard for headroom.
ALTER TABLE strategy_weights DROP CONSTRAINT IF EXISTS strategy_idx_valid;
ALTER TABLE strategy_weights
  ADD CONSTRAINT strategy_idx_valid CHECK (strategy_idx >= 0 AND strategy_idx < 16);

INSERT INTO strategy_weights_current (strategy_idx, strategy_name, weight)
VALUES (11, 'COORDINATION', 1.0)
ON CONFLICT (strategy_idx) DO NOTHING;
