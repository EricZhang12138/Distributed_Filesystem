#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

// Will be used for simulating the DS RPC calls
#include <unistd.h>
#include <sys/wait.h>
#include <dirent.h>
#include <time.h>

#include "primality_test.h"
#include "utils.h"

#define CHUNK_SIZE 100
#define HASH_SET_SIZE 100000

typedef struct
{
    char input_file[256];
    long start_line;
    long num_lines;
    int worker_id;
} Task;

void do_work(Task task)
{
    char temp_filename[256];
    snprintf(temp_filename, sizeof(temp_filename), "temp_worker_%d.txt", task.worker_id);

    // logging file creation
    char log_filename[256];
    snprintf(log_filename, sizeof(log_filename), "log_worker_%d.txt", task.worker_id);

    FILE *in_file = fopen(task.input_file, "r");
    FILE *out_file = fopen(temp_filename, "a"); // Use append mode to avoid overwriting

    if (!in_file || !out_file)
    {
        if (in_file)
            fclose(in_file);
        if (out_file)
            fclose(out_file);
        perror("Worker file open failed");
        exit(1);
    }

    char *line = NULL;
    size_t len = 0;
    long current_line = 0;

    while (current_line < task.start_line && getline(&line, &len, in_file) != -1)
    {
        current_line++;
    }

    for (int i = 0; i < task.num_lines && getline(&line, &len, in_file) != -1; ++i)
    {
        uint64_t num = strtoull(line, NULL, 10);

        if (is_prime(num))
        {
            fprintf(out_file, "%llu\n", (unsigned long long)num);
        }

        // commit worker progress
        FILE *log_file = fopen(log_filename, "w");
        if (log_file)
        {
            // worker id
            fprintf(log_file, "%d ", task.worker_id);
            // task file name
            fprintf(log_file, "%s ", task.input_file);
            // last processed line
            fprintf(log_file, "%ld ", current_line);
            // number of lines assigned
            fprintf(log_file, "%ld", task.num_lines);
        }
        fclose(log_file);
    }

    printf("Worker %d: Finished processing %s from line %ld.\n", task.worker_id, task.input_file, task.start_line);
    free(line);
    fclose(in_file);
    fclose(out_file);
}

long count_file_lines(const char *filepath)
{
    FILE *f = fopen(filepath, "r");
    if (!f)
        return 0;

    long cnt = 0;
    char buf[8192];
    while (fgets(buf, sizeof(buf), f))
    {
        cnt++;
    }
    fclose(f);
    return cnt;
}

int scan_input_files(const char *input_dir, Task *tasks, int max_tasks)
{
    DIR *d = opendir(input_dir);
    if (!d)
    {
        perror("Failed to open input directory");
        return -1;
    }

    int task_cnt = 0;
    struct dirent *entry;

    while ((entry = readdir(d)) != NULL)
    {
        if (entry->d_type != DT_REG)
            continue;

        char fullpath[512];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", input_dir, entry->d_name);

        long lines = count_file_lines(fullpath);

        for (long pos = 0; pos < lines; pos += CHUNK_SIZE)
        {
            if (task_cnt >= max_tasks)
            {
                fprintf(stderr, "Warning: Maximum task limit reached\n");
                closedir(d);
                return task_cnt;
            }
            tasks[task_cnt].start_line = pos;
            tasks[task_cnt].num_lines = CHUNK_SIZE;
            strncpy(tasks[task_cnt].input_file, fullpath, sizeof(tasks[task_cnt].input_file) - 1);
            tasks[task_cnt].input_file[sizeof(tasks[task_cnt].input_file) - 1] = '\0';
            task_cnt++;
        }
    }
    closedir(d);
    return task_cnt;
}

void collect_results(HashSet *primes_set)
{
    DIR *d = opendir(".");
    if (!d)
    {
        perror("Failed to open current directory");
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL)
    {
        if (strncmp(entry->d_name, "temp_worker_", 12) == 0)
        {
            FILE *f = fopen(entry->d_name, "r");
            if (f)
            {
                uint64_t val;
                while (fscanf(f, "%llu", (unsigned long long *)&val) == 1)
                {
                    hash_set_insert(primes_set, val);
                }
                fclose(f);
                remove(entry->d_name);
            }
        }
    }
    closedir(d);
}

void write_final_output(const char *output_file, HashSet *primes_set)
{
    FILE *f = fopen(output_file, "w");
    if (!f)
    {
        perror("Failed to open output file");
        return;
    }

    for (int idx = 0; idx < primes_set->size; ++idx)
    {
        Node *node = primes_set->table[idx];
        while (node != NULL)
        {
            fprintf(f, "%llu\n", (unsigned long long)node->key);
            node = node->next;
        }
    }
    fclose(f);
}

    // this function should be called periodicially to take master backup
void write_master_backup(const char *master_backup_file)
    /*
    This function collect the logs fromeach worker and write the current
    application status into master_backup.txt file.
    This file will be used for recovery or on restart.
    parameter:
    master_backup_file: a pointer to the backup file.
    */
{
    FILE *master = fopen(master_backup_file, "w");
    if (!master)
    {
        perror("Failed to open master backup file file");
        return;
    }

    DIR *d = opendir(".");
    if (!d)
    {
        perror("Failed to open current directory");
        fclose(master);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL)
    {
        if (strncmp(entry->d_name, "log_worker_", 11) == 0)
        {
            FILE *worker = fopen(entry->d_name, "r");
            if (worker)
            {
                char buf[256];
                if (fgets(buf, sizeof(buf), worker))
                {
                    fprintf(master, "%s\n", buf); // write latest line of that worker
                }
                fclose(worker);
            }
        }
    }
    closedir(d);
    fclose(master);
}

void load_master_backup(const char *master_backup_file, Task *tasks, int max_parallel)
{
    // load the master backup file and get the tasks
    /*
    parameters:
    master_backup_file
    tasks
    max_parallel
    */
    FILE *master_file = fopen(master_backup_file, "r");
    if (!master_file)
        return;
    int task_idx = 0;
    while (task_idx < max_parallel && !feof(master_file))
    {
        // create new task
        Task t;
        if (fscanf(master_file, "%d %255s %ld %ld", &t.worker_id, t.input_file, &t.start_line, &t.num_lines) == 4)
        {
            tasks[task_idx++] = t;
        }
    }
    printf("Loaded %d tasks from backup.\n", task_idx);
    // example of loaded task
    printf("%s %d %ld %ld \n", tasks[task_idx - 1].input_file, tasks[task_idx - 1].worker_id, tasks[task_idx - 1].start_line, tasks[task_idx - 1].num_lines);
    fclose(master_file);
}

int main(int argc, char *argv[])
{
    if (argc != 4)
    {
        fprintf(stderr, "Usage: %s <num_workers> <input_dir> <output_file>\n", argv[0]);
        return 1;
    }

    int num_workers = atoi(argv[1]);
    const char *input_dir = argv[2];
    const char *output_file = argv[3];

    if (num_workers <= 0)
    {
        fprintf(stderr, "Error: Number of workers must be positive.\n");
        return 1;
    }

    printf("Starting with %d workers...\n", num_workers);
    clock_t t_start = clock();

    Task tasks[1024];
    int task_cnt = scan_input_files(input_dir, tasks, 1024);
    if (task_cnt < 0)
        return 1;

    printf("Created %d tasks from input files.\n", task_cnt);

    int task_idx = 0;
    int workers_spawned = 0;
    int max_parallel = (num_workers < task_cnt) ? num_workers : task_cnt;

    // create master_backup
    char master_backup_filename[256];
    snprintf(master_backup_filename, sizeof(master_backup_filename), "master_backup.txt");

    for (int i = 0; i < max_parallel && task_idx < task_cnt; ++i)
    {
        pid_t pid = fork();
        if (pid == 0)
        {
            int my_idx = task_idx;
            while (my_idx < task_cnt)
            {
                tasks[my_idx].worker_id = getpid();
                do_work(tasks[my_idx]);
                my_idx += max_parallel;
            }
            exit(0);
        }
        else if (pid < 0)
        {
            perror("Fork failed");
            exit(1);
        }
        else
        {
            workers_spawned++;
            task_idx++;
        }
    }

    for (int i = 0; i < workers_spawned; ++i)
    {
        wait(NULL);
    }
    // backup master data
    write_master_backup(master_backup_filename);
    printf("All workers finished.\n");
    // load tasks from backup (will be used on restart)
    load_master_backup(master_backup_filename, tasks, max_parallel);

    printf("Aggregating results...\n");
    HashSet *unique = create_hash_set(HASH_SET_SIZE);
    collect_results(unique);
    write_final_output(output_file, unique);
    free_hash_set(unique);

    clock_t t_end = clock();
    double elapsed = (double)(t_end - t_start) / CLOCKS_PER_SEC;
    printf("Done! Results written to %s. Time: %.2f seconds.\n", output_file, elapsed);

    return 0;
}