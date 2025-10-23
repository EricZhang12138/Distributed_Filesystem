# Prime Finder

A parallel prime number finder using multi-process architecture.

## Testing

Generate test data:
```bash
make setup
```

Build the program:
```bash
make clean && make
```

Run the program:
```bash
./prime_finder <num_workers> <input_dir> <output_file>
```

Example:
```bash
./prime_finder 4 ./test_data/ output.txt
```

Parameters:
- `num_workers`: Number of parallel worker processes (e.g., 4)
- `input_dir`: Directory containing input files with numbers
- `output_file`: Path to output file for unique primes

## How It Works

1. **Coordinator** reads all files in the input directory and divides them into chunks
2. **Workers** (forked processes) process chunks in parallel, testing each number for primality
3. Each worker writes discovered primes to a temporary file
4. **Coordinator** aggregates results, removes duplicates using a hash set, and writes unique primes to the output file

## Program Flow

```
Input Files → Task Creation → Worker Pool → Parallel Processing → Aggregation → Output
```

- **Input**: Directory containing text files with numbers (one per line)
- **Processing**: Each worker tests numbers using trial division
- **Output**: Single file containing all unique prime numbers found

