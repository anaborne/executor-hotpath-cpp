# For a non-technical reader

This project is a rewrite in C++ of one program taken from a Python trading system. The program sits
on a local connection, reads an incoming order message, checks it, replies, and records how long
that took. The rewrite keeps the exact message format of the Python original, so the original Python
client drives either version without being changed, and in the benchmark run it did so across 4400
messages with every one accepted. The time the program spent handling a message fell from 0.0045
milliseconds to 0.0016 milliseconds at the median, a factor of about 2.8, and about 2.4 at the slow
end of the range. That comparison also moved the program into a separate process, so part of the
gain belongs to the move and part to the language change, and the repository says the two cannot be
separated from this data. Anyone can build the project, run its 92 tests, and check the published
figures against the raw file `bench_history.csv`.
