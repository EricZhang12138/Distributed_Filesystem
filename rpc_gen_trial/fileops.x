const FILE_BLOCK = 4096;

enum errors {RESTRICTED_USER, ALL};

struct file_chunk {
	int last;
	int size;
	errors err;
	opaque data[FILE_BLOCK];
};

struct file_chunk_info {
	string filePath<255>;
	int offset;
};

struct file_flush_data {
	opaque data[FILE_BLOCK];
	string filePath<255>;
	int size;
	int last;
	int offset;
};

program FILEOPENPROG {
	version FILEOPENVERS {
		file_chunk FILEOPEN(file_chunk_info) = 1;
	} = 1;
} = 0x30000000;

program FILECLOSEPROG {
	version FILECLOSEVERS {
		file_chunk FILECLOSE(file_flush_data) = 1;
	} = 1;
} = 0x30000001;

program FILEREADPROG {
	version FILEREADVERS {
		file_chunk FILEREAD(file_chunk_info) = 1;
	} = 1; 
} = 0x30000002;

program FILECREATEPROG {
	version FILECREATEVERS {
		file_chunk FILECREATE(file_chunk_info) = 1;
	} = 1;
} = 0x30000003;

