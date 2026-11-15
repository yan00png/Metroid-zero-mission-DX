#ifndef STORAGE_H
#define STORAGE_H
/* Armazenamento: pen-drive USB (FAT32/exFAT via bdm+bdmfs_fatfs) lido pelo fileXio. */
void StorageResetIop(const char *argv0); /* chamar antes de tudo */
int StorageInit(const char *argv0);   /* carrega drivers, espera o pen-drive; retorna 1 se montou */
int StorageMount(int tries);          /* tenta montar de novo (tries x 0,1 s) */
int StorageReady(void);
const char *StorageUsbDev(void);
int StorageIsDisc(void);              /* 1 se o ELF foi iniciado de CD/DVD/ISO (ROM no disco, save no memory card) */
int StorageFileSize(const char *rel); /* bytes ou -1 */
const char *StorageStatus(void);      /* texto curto de diagnostico */
const char *StorageBaseDir(void);     /* pasta do ELF, ex.: "mass0:/MZM/" */
/* IO de arquivo (caminho relativo a StorageBaseDir()) */
int StorageOpen(const char *rel, int write);   /* fd >= 0 ou < 0 */
int StorageRead(int fd, void *buf, int n);
int StorageWrite(int fd, const void *buf, int n);
int StorageSeek(int fd, int off, int whence);
void StorageClose(int fd);
int StorageMkdir(const char *rel);
#endif
