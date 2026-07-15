// tune.h — Part 11: Texel tuning
#ifndef TUNE_H
#define TUNE_H

// Dataset: one "FEN;result" line per position, result in {1, 0.5, 0}
// from White's perspective. Prints tuned tables as C source on completion.
void tune_run(const char *dataset_path);

#endif
