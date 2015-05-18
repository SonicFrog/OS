#ifndef H_UTIL
#define H_UTIL

void* mmap_file(int fd, off_t offset, size_t size);
void unmap(void* buf, size_t size);


#define MIN(a, b) (((a) > (b)) ? (b) : (a))

#endif
