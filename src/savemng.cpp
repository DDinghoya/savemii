#include <cstdio>
#include <nn/act/client_cpp.h>
#include <string.hpp>

#include <LockingQueue.h>
#include <future>
#include <language.h>
#include <savemng.h>

#define IO_MAX_FILE_BUFFER (1024 * 1024) // 1 MB

static char *p1;
Account *wiiuacc;
Account *sdacc;
uint8_t wiiuaccn = 0, sdaccn = 5;

extern FSClient *__wut_devoptab_fs_client;
static FSCmdBlock cmdBlk;

typedef struct {
    void *buf;
    size_t len;
    size_t buf_size;
} file_buffer;

static file_buffer buffers[16];
static char *fileBuf[2];
static bool buffersInitialized = false;

bool initFS() {
    FSInit();
    FSInitCmdBlock(&cmdBlk);
    FSSetCmdPriority(&cmdBlk, 0);
    bool ret = Mocha_InitLibrary() == MOCHA_RESULT_SUCCESS;
    if (ret)
        ret = Mocha_UnlockFSClient(__wut_devoptab_fs_client) == MOCHA_RESULT_SUCCESS;
    if (ret) {
        Mocha_MountFS("slc", "/dev/slccmpt01", "/vol/storage_slccmpt01");
        fatMountSimple("sd", &Mocha_sdio_disc_interface);
        return true;
    }
    return false;
}

void deinitFS() {
    fatUnmount("sd");
    Mocha_UnmountFS("slc");
    Mocha_DeInitLibrary();
    FSShutdown();
}

static void showFileOperation(std::string file_name, std::string file_src, std::string file_dest) {
    consolePrintPos(-2, 0, gettext("Copying file: %s"), file_name.c_str());
    consolePrintPosMultiline(-2, 2, '/', gettext("From: %s"), file_src.c_str());
    consolePrintPosMultiline(-2, 8, '/', gettext("To: %s"), file_dest.c_str());
}

int32_t loadFile(const char *fPath, uint8_t **buf) {
    int ret = 0;
    FILE *file = fopen(fPath, "rb");
    if (file != nullptr) {
        struct stat st;
        stat(fPath, &st);
        int size = st.st_size;

        *buf = (uint8_t *) malloc(size);
        if (*buf != nullptr) {
            if (fread(*buf, size, 1, file) == 1)
                ret = size;
            else
                free(*buf);
        }
        fclose(file);
    }
    return ret;
}

static int32_t loadFilePart(const char *fPath, uint32_t start, uint32_t size, uint8_t **buf) {
    int ret = 0;
    FILE *file = fopen(fPath, "rb");
    if (file != nullptr) {
        struct stat st;
        stat(fPath, &st);
        if ((start + size) > st.st_size) {
            fclose(file);
            return -43;
        }
        if (fseek(file, start, SEEK_SET) == -1) {
            fclose(file);
            return -43;
        }

        *buf = (uint8_t *) malloc(size);
        if (*buf != nullptr) {
            if (fread(*buf, size, 1, file) == 1)
                ret = size;
            else
                free(*buf);
        }
        fclose(file);
    }
    return ret;
}

int32_t loadTitleIcon(Title *title) {
    uint32_t highID = title->highID;
    uint32_t lowID = title->lowID;
    bool isUSB = title->isTitleOnUSB;
    bool isWii = ((highID & 0xFFFFFFF0) == 0x00010000);
    std::string path;

    if (isWii) {
        if (title->saveInit) {
            path = stringFormat("/vol/storage_slccmpt01/title/%08x/%08x/data/banner.bin", highID, lowID);
            return loadFilePart(path.c_str(), 0xA0, 24576, &title->iconBuf);
        }
    } else {
        if (title->saveInit)
            path = stringFormat("%s/usr/save/%08x/%08x/meta/iconTex.tga", isUSB ? "/vol/storage_usb01" : "/vol/storage_mlc01", highID, lowID);
        else
            path = stringFormat("%s/usr/title/%08x/%08x/meta/iconTex.tga", isUSB ? "/vol/storage_usb01" : "/vol/storage_mlc01", highID, lowID);

        return loadFile(path.c_str(), &title->iconBuf);
    }
    return -23;
}

int checkEntry(const char *fPath) {
    struct stat st;
    if (stat(fPath, &st) == -1)
        return 0;

    if (S_ISDIR(st.st_mode))
        return 2;

    return 1;
}

static bool folderEmpty(const char *fPath) {
    DIR *dir = opendir(fPath);
    if (dir == nullptr)
        return false;

    int c = 0;
    struct dirent *data;
    while ((data = readdir(dir)) != nullptr)
        if (++c > 2)
            break;

    closedir(dir);
    return c < 3 ? true : false;
}

static bool createFolder(const char *fPath) { //Adapted from mkdir_p made by JonathonReinhart
    std::string _path;
    char *p;
    int found = 0;

    _path.assign(fPath);

    for (p = (char *) _path.c_str() + 1; *p != 0; p++) {
        if (*p == '/') {
            found++;
            if (found > 2) {
                *p = '\0';
                if (checkEntry(_path.c_str()) == 0)
                    if (mkdir(_path.c_str(), DEFFILEMODE) == -1)
                        return false;
                *p = '/';
            }
        }
    }

    if (checkEntry(_path.c_str()) == 0)
        if (mkdir(_path.c_str(), DEFFILEMODE) == -1)
            return false;

    return true;
}

void consolePrintPosAligned(int y, uint16_t offset, uint8_t align, const char *format, ...) {
    char *tmp = NULL;
    int x = 0;

    va_list va;
    va_start(va, format);
    if ((vasprintf(&tmp, format, va) >= 0) && tmp) {
        switch (align) {
            case 0:
                x = (offset * 12);
                break;
            case 1:
                x = (853 - ttfStringWidth((char *) tmp, -2)) / 2;
                break;
            case 2:
                x = 853 - (offset * 12) - ttfStringWidth((char *) tmp, 0);
                break;
            default:
                x = (853 - ttfStringWidth((char *) tmp, -2)) / 2;
                break;
        }
        ttfPrintString(x, (y + 1) * 24, (char *) tmp, false, false);
    }
    va_end(va);
    if (tmp) free(tmp);
}

void consolePrintPos(int x, int y, const char *format, ...) { // Source: ftpiiu
    char *tmp = nullptr;

    va_list va;
    va_start(va, format);
    if ((vasprintf(&tmp, format, va) >= 0) && (tmp != nullptr))
        ttfPrintString((x + 4) * 12, (y + 1) * 24, tmp, false, true);
    va_end(va);
    if (tmp != nullptr)
        free(tmp);
}

void consolePrintPosMultiline(int x, int y, char cdiv, const char *format, ...) { // Source: ftpiiu
    char *tmp = nullptr;
    uint32_t len = (66 - x);

    va_list va;
    va_start(va, format);
    if ((vasprintf(&tmp, format, va) >= 0) && (tmp != nullptr)) {
        if ((uint32_t) (ttfStringWidth(tmp, -1) / 12) > len) {
            char *p = tmp;
            if (strrchr(p, '\n') != nullptr)
                p = strrchr(p, '\n') + 1;
            while ((uint32_t) (ttfStringWidth(p, -1) / 12) > len) {
                char *q = p;
                int l1 = strlen(q);
                for (int i = l1; i > 0; i--) {
                    char o = q[l1];
                    q[l1] = '\0';
                    if ((uint32_t) (ttfStringWidth(p, -1) / 12) <= len) {
                        if (strrchr(p, cdiv) != nullptr)
                            p = strrchr(p, cdiv) + 1;
                        else
                            p = q + l1;
                        q[l1] = o;
                        break;
                    }
                    q[l1] = o;
                    l1--;
                }
                std::string buf;
                buf.assign(p);
                p = (char *) stringFormat("\n%s", buf.c_str()).c_str();
                p++;
                len = 69;
            }
        }
        ttfPrintString((x + 4) * 12, (y + 1) * 24, tmp, true, true);
    }
    va_end(va);
    if (tmp != nullptr)
        free(tmp);
}

bool promptConfirm(Style st, std::string question) {
    clearBuffers();
    WHBLogFreetypeDraw();
    const std::string msg1 = gettext("\ue000 Yes - \ue001 No");
    const std::string msg2 = gettext("\ue000 Confirm - \ue001 Cancel");
    std::string msg;
    switch (st & 0x0F) {
        case ST_YES_NO:
            msg = msg1;
            break;
        case ST_CONFIRM_CANCEL:
            msg = msg2;
            break;
        default:
            msg = msg2;
    }
    if (st & ST_WARNING) {
        OSScreenClearBufferEx(SCREEN_TV, 0x7F7F0000);
        OSScreenClearBufferEx(SCREEN_DRC, 0x7F7F0000);
    } else if (st & ST_ERROR) {
        OSScreenClearBufferEx(SCREEN_TV, 0x7F000000);
        OSScreenClearBufferEx(SCREEN_DRC, 0x7F000000);
    } else {
        OSScreenClearBufferEx(SCREEN_TV, 0x007F0000);
        OSScreenClearBufferEx(SCREEN_DRC, 0x007F0000);
    }
    if (!(st & ST_MULTILINE)) {
        consolePrintPos(31 - (ttfStringWidth((char *) question.c_str(), 0) / 24), 7, question.c_str());
        consolePrintPos(31 - (ttfStringWidth((char *) msg.c_str(), -1) / 24), 9, msg.c_str());
    }

    int ret = 0;
    flipBuffers();
    WHBLogFreetypeDraw();
    while (true) {
        readInput();
        if (getInput(TRIGGER, PAD_BUTTON_A)) {
            ret = 1;
            break;
        }
        if (getInput(TRIGGER, PAD_BUTTON_B)) {
            ret = 0;
            break;
        }
    }
    return ret != 0;
}

void promptError(const char *message, ...) {
    clearBuffers();
    WHBLogFreetypeDraw();
    va_list va;
    va_start(va, message);
    clearBuffersEx();
    char *tmp = nullptr;
    if ((vasprintf(&tmp, message, va) >= 0) && (tmp != nullptr)) {
        int x = 31 - (ttfStringWidth(tmp, -2) / 24);
        int y = 8;
        x = (x < -4 ? -4 : x);
        ttfPrintString((x + 4) * 12, (y + 1) * 24, tmp, true, false);
    }
    if (tmp != nullptr)
        free(tmp);
    flipBuffers();
    WHBLogFreetypeDraw();
    va_end(va);
    sleep(2);
}

void getAccountsWiiU() {
    /* get persistent ID - thanks to Maschell */
    nn::act::Initialize();
    int i = 0;
    int accn = 0;
    wiiuaccn = nn::act::GetNumOfAccounts();
    wiiuacc = (Account *) malloc(wiiuaccn * sizeof(Account));
    uint16_t out[11];
    while ((accn < wiiuaccn) && (i <= 12)) {
        if (nn::act::IsSlotOccupied(i)) {
            unsigned int persistentID = nn::act::GetPersistentIdEx(i);
            wiiuacc[accn].pID = persistentID;
            sprintf(wiiuacc[accn].persistentID, "%08X", persistentID);
            nn::act::GetMiiNameEx((int16_t *) out, i);
            memset(wiiuacc[accn].miiName, 0, sizeof(wiiuacc[accn].miiName));
            for (int j = 0, k = 0; j < 10; j++) {
                if (out[j] < 0x80) {
                    wiiuacc[accn].miiName[k++] = (char) out[j];
                } else if ((out[j] & 0xF000) > 0) {
                    wiiuacc[accn].miiName[k++] = 0xE0 | ((out[j] & 0xF000) >> 12);
                    wiiuacc[accn].miiName[k++] = 0x80 | ((out[j] & 0xFC0) >> 6);
                    wiiuacc[accn].miiName[k++] = 0x80 | (out[j] & 0x3F);
                } else if (out[j] < 0x400) {
                    wiiuacc[accn].miiName[k++] = 0xC0 | ((out[j] & 0x3C0) >> 6);
                    wiiuacc[accn].miiName[k++] = 0x80 | (out[j] & 0x3F);
                } else {
                    wiiuacc[accn].miiName[k++] = 0xD0 | ((out[j] & 0x3C0) >> 6);
                    wiiuacc[accn].miiName[k++] = 0x80 | (out[j] & 0x3F);
                }
            }
            wiiuacc[accn].slot = i;
            accn++;
        }
        i++;
    }
    nn::act::Finalize();
}

void getAccountsSD(Title *title, uint8_t slot) {
    uint32_t highID = title->highID;
    uint32_t lowID = title->lowID;
    sdaccn = 0;
    if (sdacc != nullptr)
        free(sdacc);

    std::string path = stringFormat("sd:/wiiu/backups/%08x%08x/%u", highID, lowID, slot);
    DIR *dir = opendir(path.c_str());
    if (dir != nullptr) {
        struct dirent *data;
        while ((data = readdir(dir)) != nullptr) {
            if (data->d_name[0] == '.' || data->d_name[0] == 's' || strncmp(data->d_name, "common", 6) == 0)
                continue;
            sdaccn++;
        }
        closedir(dir);
    }

    sdacc = (Account *) malloc(sdaccn * sizeof(Account));
    dir = opendir(path.c_str());
    if (dir != nullptr) {
        struct dirent *data;
        int i = 0;
        while ((data = readdir(dir)) != nullptr) {
            if (data->d_name[0] == '.' || data->d_name[0] == 's' || strncmp(data->d_name, "common", 6) == 0)
                continue;
            sprintf(sdacc[i].persistentID, "%s", data->d_name);
            sdacc[i].pID = strtoul(data->d_name, nullptr, 16);
            sdacc[i].slot = i;
            i++;
        }
        closedir(dir);
    }
}

static bool readThread(FILE *srcFile, LockingQueue<file_buffer> *ready, LockingQueue<file_buffer> *done) {
    file_buffer currentBuffer;
    ready->waitAndPop(currentBuffer);
    while ((currentBuffer.len = fread(currentBuffer.buf, 1, currentBuffer.buf_size, srcFile)) > 0) {
        done->push(currentBuffer);
        ready->waitAndPop(currentBuffer);
    }
    done->push(currentBuffer);
    return ferror(srcFile) == 0;
}

static bool writeThread(FILE *dstFile, LockingQueue<file_buffer> *ready, LockingQueue<file_buffer> *done, size_t totalSize) {
    uint bytes_written;
    size_t total_bytes_written = 0;
    file_buffer currentBuffer;
    ready->waitAndPop(currentBuffer);
    while (currentBuffer.len > 0 && (bytes_written = fwrite(currentBuffer.buf, 1, currentBuffer.len, dstFile)) == currentBuffer.len) {
        done->push(currentBuffer);
        ready->waitAndPop(currentBuffer);
        total_bytes_written += bytes_written;
    }
    done->push(currentBuffer);
    return ferror(dstFile) == 0;
}

static bool copyFileThreaded(FILE *srcFile, FILE *dstFile, size_t totalSize) {
    LockingQueue<file_buffer> read;
    LockingQueue<file_buffer> write;
    for (auto &buffer : buffers) {
        if (!buffersInitialized) {
            buffer.buf = aligned_alloc(0x40, IO_MAX_FILE_BUFFER);
            buffer.len = 0;
            buffer.buf_size = IO_MAX_FILE_BUFFER;
        }
        read.push(buffer);
    }
    if (!buffersInitialized) {
        fileBuf[0] = static_cast<char *>(aligned_alloc(0x40, IO_MAX_FILE_BUFFER));
        fileBuf[1] = static_cast<char *>(aligned_alloc(0x40, IO_MAX_FILE_BUFFER));
    }
    buffersInitialized = true;
    setvbuf(srcFile, fileBuf[0], _IOFBF, IO_MAX_FILE_BUFFER);
    setvbuf(dstFile, fileBuf[1], _IOFBF, IO_MAX_FILE_BUFFER);

    std::future<bool> readFut = std::async(std::launch::async, readThread, srcFile, &read, &write);
    std::future<bool> writeFut = std::async(std::launch::async, writeThread, dstFile, &write, &read, totalSize);
    bool success = readFut.get() && writeFut.get();
    return success;
}

static bool copyFile(std::string pPath, std::string oPath) {
    if (pPath.find("savemiiMeta.json") != std::string::npos)
        return true;
    FILE *source = fopen(pPath.c_str(), "rb");
    if (source == nullptr)
        return false;

    FILE *dest = fopen(oPath.c_str(), "wb");
    if (dest == nullptr) {
        fclose(source);
        return false;
    }

    struct stat st;
    if (stat(pPath.c_str(), &st) < 0)
        return false;
    int sizef = st.st_size;

    clearBuffersEx();
    showFileOperation(basename(pPath.c_str()), pPath, oPath);
    consolePrintPos(-2, 15, gettext("Filesize: %d bytes"), sizef);
    flipBuffers();
    WHBLogFreetypeDraw();

    copyFileThreaded(source, dest, sizef);

    FSChangeMode(__wut_devoptab_fs_client, &cmdBlk, (char *) oPath.c_str(), (FSMode) 0x666, (FSMode) 0x777, FS_ERROR_FLAG_ALL);

    fclose(source);
    fclose(dest);

    return true;
}

static int copyDir(std::string pPath, std::string tPath) { // Source: ft2sd
    DIR *dir = opendir(pPath.c_str());
    if (dir == nullptr)
        return -1;

    mkdir(tPath.c_str(), DEFFILEMODE);
    auto *data = (dirent *) malloc(sizeof(dirent));

    while ((data = readdir(dir)) != nullptr) {
        clearBuffersEx();

        if (strcmp(data->d_name, "..") == 0 || strcmp(data->d_name, ".") == 0)
            continue;

        std::string targetPath = stringFormat("%s/%s", tPath.c_str(), data->d_name);

        if ((data->d_type & DT_DIR) != 0) {
            mkdir(targetPath.c_str(), DEFFILEMODE);
            if (copyDir(pPath + stringFormat("/%s", data->d_name), targetPath) != 0) {
                closedir(dir);
                return -2;
            }
        } else {
            p1 = data->d_name;
            showFileOperation(data->d_name, pPath + stringFormat("/%s", data->d_name), targetPath);

            if (!copyFile(pPath + stringFormat("/%s", data->d_name), targetPath)) {
                closedir(dir);
                return -3;
            }
        }
    }

    closedir(dir);

    return 0;
}

static bool removeDir(char *pPath) {
    DIR *dir = opendir(pPath);
    if (dir == NULL)
        return false;

    struct dirent *data;

    while ((data = readdir(dir)) != NULL) {
        clearBuffersEx();

        if (strcmp(data->d_name, "..") == 0 || strcmp(data->d_name, ".") == 0) continue;

        int len = strlen(pPath);
        snprintf(pPath + len, PATH_MAX - len, "/%s", data->d_name);

        if (data->d_type & DT_DIR) {
            char origPath[PATH_SIZE];
            sprintf(origPath, "%s", pPath);
            removeDir(pPath);

            clearBuffersEx();

            consolePrintPos(-2, 0, gettext("Deleting folder %s"), data->d_name);
            consolePrintPosMultiline(-2, 2, '/', gettext("From: \n%s"), origPath);
            if (unlink(origPath) == -1) promptError(gettext("Failed to delete folder %s\n%s"), origPath, strerror(errno));
        } else {
            consolePrintPos(-2, 0, gettext("Deleting file %s"), data->d_name);
            consolePrintPosMultiline(-2, 2, '/', gettext("From: \n%s"), pPath);
            if (unlink(pPath) == -1) promptError(gettext("Failed to delete file %s\n%s"), pPath, strerror(errno));
        }

        flipBuffers();
        WHBLogFreetypeDraw();
        pPath[len] = 0;
    }

    closedir(dir);
    return true;
}

static std::string getUserID() { // Source: loadiine_gx2
    /* get persistent ID - thanks to Maschell */
    nn::act::Initialize();

    unsigned char slotno = nn::act::GetSlotNo();
    unsigned int persistentID = nn::act::GetPersistentIdEx(slotno);
    nn::act::Finalize();
    std::string out = stringFormat("%08X", persistentID);
    return out;
}

int getLoadiineGameSaveDir(char *out, const char *productCode, const char *longName, const uint32_t highID, const uint32_t lowID) {
    DIR *dir = opendir("sd:/wiiu/saves");

    if (dir == nullptr)
        return -1;

    struct dirent *data;
    while ((data = readdir(dir)) != nullptr) {
        if (((data->d_type & DT_DIR) != 0) && ((strstr(data->d_name, productCode) != nullptr) || (strstr(data->d_name, stringFormat("%s [%08x%08x]", longName, highID, lowID).c_str()) != nullptr))) {
            sprintf(out, "sd:/wiiu/saves/%s", data->d_name);
            closedir(dir);
            return 0;
        }
    }

    promptError(gettext("Loadiine game folder not found."));
    closedir(dir);
    return -2;
}

bool getLoadiineSaveVersionList(int *out, const char *gamePath) {
    DIR *dir = opendir(gamePath);

    if (dir == nullptr) {
        promptError(gettext("Loadiine game folder not found."));
        return false;
    }

    int i = 0;
    struct dirent *data;
    while (i < 255 && (data = readdir(dir)) != nullptr)
        if (((data->d_type & DT_DIR) != 0) && (strchr(data->d_name, 'v') != nullptr))
            out[++i] = strtol((data->d_name) + 1, nullptr, 10);

    closedir(dir);
    return true;
}

static bool getLoadiineUserDir(char *out, const char *fullSavePath, const char *userID) {
    DIR *dir = opendir(fullSavePath);

    if (dir == nullptr) {
        promptError(gettext("Failed to open Loadiine game save directory."));
        return false;
    }

    struct dirent *data;
    while ((data = readdir(dir)) != nullptr) {
        if (((data->d_type & DT_DIR) != 0) && ((strstr(data->d_name, userID)) != nullptr)) {
            sprintf(out, "%s/%s", fullSavePath, data->d_name);
            closedir(dir);
            return true;
        }
    }

    sprintf(out, "%s/u", fullSavePath);
    closedir(dir);
    if (checkEntry(out) <= 0)
        return false;
    return true;
}

bool isSlotEmpty(uint32_t highID, uint32_t lowID, uint8_t slot) {
    std::string path;
    if (((highID & 0xFFFFFFF0) == 0x00010000) && (slot == 255))
        path = stringFormat("sd:/savegames/%08x%08x", highID, lowID);
    else
        path = stringFormat("sd:/wiiu/backups/%08x%08x/%u", highID, lowID, slot);
    int ret = checkEntry(path.c_str());
    return ret <= 0;
}

static int getEmptySlot(uint32_t highID, uint32_t lowID) {
    for (int i = 0; i < 256; i++)
        if (isSlotEmpty(highID, lowID, i))
            return i;
    return -1;
}

bool hasAccountSave(Title *title, bool inSD, bool iine, uint32_t user, uint8_t slot, int version) {
    uint32_t highID = title->highID;
    uint32_t lowID = title->lowID;
    bool isUSB = title->isTitleOnUSB;
    bool isWii = ((highID & 0xFFFFFFF0) == 0x00010000);
    if (highID == 0 || lowID == 0)
        return false;

    char srcPath[PATH_SIZE];
    if (!isWii) {
        if (!inSD) {
            const char *path = (isUSB ? "/vol/storage_usb01/usr/save" : "/vol/storage_mlc01/usr/save");
            if (user == 0) {
                sprintf(srcPath, "%s/%08x/%08x/%s/common", path, highID, lowID, "user");
            } else if (user == 0xFFFFFFFF) {
                sprintf(srcPath, "%s/%08x/%08x/%s", path, highID, lowID, "user");
            } else {
                sprintf(srcPath, "%s/%08x/%08x/%s/%08X", path, highID, lowID, "user", user);
            }
        } else {
            if (!iine) {
                sprintf(srcPath, "sd:/wiiu/backups/%08x%08x/%u/%08X", highID, lowID, slot, user);
            } else {
                if (getLoadiineGameSaveDir(srcPath, title->productCode, title->longName, title->highID, title->lowID) != 0) {
                    return false;
                }
                if (version != 0) {
                    sprintf(srcPath + strlen(srcPath), "/v%u", version);
                }
                if (user == 0) {
                    uint32_t srcOffset = strlen(srcPath);
                    strcpy(srcPath + srcOffset, "/c\0");
                } else {
                    char usrPath[16];
                    sprintf(usrPath, "%08X", user);
                    getLoadiineUserDir(srcPath, srcPath, usrPath);
                }
            }
        }
    } else {
        if (!inSD) {
            sprintf(srcPath, "/vol/storage_slccmpt01/title/%08x/%08x/data", highID, lowID);
        } else {
            sprintf(srcPath, "sd:/wiiu/backups/%08x%08x/%u", highID, lowID, slot);
        }
    }
    if (checkEntry(srcPath) == 2) {
        if (!folderEmpty(srcPath)) {
            return true;
        }
    }
    return false;
}

bool hasCommonSave(Title *title, bool inSD, bool iine, uint8_t slot, int version) {
    uint32_t highID = title->highID;
    uint32_t lowID = title->lowID;
    bool isUSB = title->isTitleOnUSB;
    bool isWii = ((highID & 0xFFFFFFF0) == 0x00010000);
    if (isWii)
        return false;

    std::string srcPath;
    if (!inSD) {
        const char *path = (isUSB ? "/vol/storage_usb01/usr/save" : "/vol/storage_mlc01/usr/save");
        srcPath = stringFormat("%s/%08x/%08x/%s/common", path, highID, lowID, "user");
    } else {
        if (!iine) {
            srcPath = stringFormat("sd:/wiiu/backups/%08x%08x/%u/common", highID, lowID, slot);
        } else {
            if (getLoadiineGameSaveDir(srcPath.data(), title->productCode, title->longName, title->highID, title->lowID) != 0)
                return false;
            if (version != 0)
                srcPath.append(stringFormat("/v%u", version));
            srcPath.append("/c\0");
        }
    }
    if (checkEntry(srcPath.c_str()) == 2)
        if (!folderEmpty(srcPath.c_str()))
            return true;
    return false;
}

void copySavedata(Title *title, Title *titleb, int8_t allusers, int8_t allusers_d, bool common) {
    uint32_t highID = title->highID;
    uint32_t lowID = title->lowID;
    bool isUSB = title->isTitleOnUSB;
    uint32_t highIDb = titleb->highID;
    uint32_t lowIDb = titleb->lowID;
    bool isUSBb = titleb->isTitleOnUSB;

    if (!promptConfirm(ST_WARNING, gettext("Are you sure?")))
        return;
    int slotb = getEmptySlot(titleb->highID, titleb->lowID);
    if ((slotb >= 0) && promptConfirm(ST_YES_NO, gettext("Backup current savedata first to next empty slot?"))) {
        backupSavedata(titleb, slotb, allusers, common);
        promptError(gettext("Backup done. Now copying Savedata."));
    }

    std::string path = (isUSB ? "/vol/storage_usb01/usr/save" : "/vol/storage_mlc01/usr/save");
    std::string pathb = (isUSBb ? "/vol/storage_usb01/usr/save" : "/vol/storage_mlc01/usr/save");
    std::string srcPath = stringFormat("%s/%08x/%08x/%s", path.c_str(), highID, lowID, "user");
    std::string dstPath = stringFormat("%s/%08x/%08x/%s", pathb.c_str(), highIDb, lowIDb, "user");
    createFolder(dstPath.c_str());

    if (allusers > -1)
        if (common)
            if (copyDir(srcPath + "/common", dstPath + "/common") != 0)
                promptError(gettext("Common save not found."));

    if (copyDir(srcPath + stringFormat("/%s", wiiuacc[allusers].persistentID),
                dstPath + stringFormat("/%s", wiiuacc[allusers_d].persistentID)) != 0)
        promptError(gettext("Copy failed."));
}

void backupAllSave(Title *titles, int count, OSCalendarTime *date) {
    OSCalendarTime dateTime;
    if (date) {
        if (date->tm_year == 0) {
            OSTicksToCalendarTime(OSGetTime(), date);
            date->tm_mon++;
        }
        dateTime = (*date);
    } else {
        OSTicksToCalendarTime(OSGetTime(), &dateTime);
        dateTime.tm_mon++;
    }

    std::string datetime = stringFormat("%04d-%02d-%02dT%02d%02d%02d", dateTime.tm_year, dateTime.tm_mon, dateTime.tm_mday,
                                        dateTime.tm_hour, dateTime.tm_min, dateTime.tm_sec);
    for (int i = 0; i < count; i++) {
        if (titles[i].highID == 0 || titles[i].lowID == 0 || !titles[i].saveInit)
            continue;

        uint32_t highID = titles[i].highID;
        uint32_t lowID = titles[i].lowID;
        bool isUSB = titles[i].isTitleOnUSB;
        bool isWii = ((highID & 0xFFFFFFF0) == 0x00010000);
        const std::string path = (isWii ? "/vol/storage_slccmpt01/title" : (isUSB ? "/vol/storage_usb01/usr/save" : "/vol/storage_mlc01/usr/save"));
        std::string srcPath = stringFormat("%s/%08x/%08x/%s", path.c_str(), highID, lowID, isWii ? "data" : "user");
        std::string dstPath = stringFormat("sd:/wiiu/backups/batch/%s/%08x%08x", datetime.c_str(), highID, lowID);

        createFolder(dstPath.c_str());
        if (copyDir(srcPath, dstPath) != 0)
            promptError(gettext("Backup failed."));
    }
}

void backupSavedata(Title *title, uint8_t slot, int8_t allusers, bool common) {
    if (!isSlotEmpty(title->highID, title->lowID, slot) &&
        !promptConfirm(ST_WARNING, gettext("Backup found on this slot. Overwrite it?"))) {
        return;
    }
    uint32_t highID = title->highID;
    uint32_t lowID = title->lowID;
    bool isUSB = title->isTitleOnUSB;
    bool isWii = ((highID & 0xFFFFFFF0) == 0x00010000);
    const std::string path = (isWii ? "/vol/storage_slccmpt01/title" : (isUSB ? "/vol/storage_usb01/usr/save" : "/vol/storage_mlc01/usr/save"));
    std::string srcPath = stringFormat("%s/%08x/%08x/%s", path.c_str(), highID, lowID, isWii ? "data" : "user");
    std::string dstPath;
    if (isWii && (slot == 255))
        dstPath = stringFormat("sd:/savegames/%08x%08x", highID, lowID);
    else
        dstPath = stringFormat("sd:/wiiu/backups/%08x%08x/%u", highID, lowID, slot);
    createFolder(dstPath.c_str());

    if ((allusers > -1) && !isWii) {
        if (common) {
            srcPath.append("/common");
            dstPath.append("/common");
            if (copyDir(srcPath, dstPath) != 0)
                promptError(gettext("Common save not found."));
        }
        srcPath.append(stringFormat("/%s", wiiuacc[allusers].persistentID));
        dstPath.append(stringFormat("/%s", wiiuacc[allusers].persistentID));
        if (checkEntry(srcPath.c_str()) == 0) {
            promptError(gettext("No save found for this user."));
            return;
        }
    }
    if (copyDir(srcPath, dstPath) != 0)
        promptError(gettext("Backup failed. DO NOT restore from this slot."));
    OSCalendarTime now;
    OSTicksToCalendarTime(OSGetTime(), &now);
    std::string date = stringFormat("%02d/%02d/%d %02d:%02d", now.tm_mday, now.tm_mon, now.tm_year, now.tm_hour, now.tm_min);
    setSlotDate(title->highID, title->lowID, slot, date);
}

void restoreSavedata(Title *title, uint8_t slot, int8_t sdusers, int8_t allusers, bool common) {
    if (isSlotEmpty(title->highID, title->lowID, slot)) {
        promptError(gettext("No backup found on selected slot."));
        return;
    }
    if (!promptConfirm(ST_WARNING, gettext("Are you sure?")))
        return;
    int slotb = getEmptySlot(title->highID, title->lowID);
    if ((slotb >= 0) && promptConfirm(ST_YES_NO, gettext("Backup current savedata first to next empty slot?")))
        backupSavedata(title, slotb, allusers, common);
    uint32_t highID = title->highID;
    uint32_t lowID = title->lowID;
    bool isUSB = title->isTitleOnUSB;
    bool isWii = ((highID & 0xFFFFFFF0) == 0x00010000);
    std::string srcPath;
    const std::string path = (isWii ? "/vol/storage_slccmpt01/title" : (isUSB ? "/vol/storage_usb01/usr/save" : "/vol/storage_mlc01/usr/save"));
    if (isWii && (slot == 255))
        srcPath = stringFormat("sd:/savegames/%08x%08x", highID, lowID);
    else
        srcPath = stringFormat("sd:/wiiu/backups/%08x%08x/%u", highID, lowID, slot);
    std::string dstPath = stringFormat("%s/%08x/%08x/%s", path.c_str(), highID, lowID, isWii ? "data" : "user");
    createFolder(dstPath.c_str());

    if ((sdusers > -1) && !isWii) {
        if (common) {
            srcPath.append("/common");
            dstPath.append("/common");
            if (copyDir(srcPath, dstPath) != 0)
                promptError(gettext("Common save not found."));
        }
        srcPath.append(stringFormat("/%s", sdacc[sdusers].persistentID));
        dstPath.append(stringFormat("/%s", wiiuacc[allusers].persistentID));
    }

    if (copyDir(srcPath, dstPath) != 0)
        promptError(gettext("Restore failed."));
}

void wipeSavedata(Title *title, int8_t allusers, bool common) {
    if (!promptConfirm(ST_WARNING, gettext("Are you sure?")) || !promptConfirm(ST_WARNING, gettext("Hm, are you REALLY sure?")))
        return;
    int slotb = getEmptySlot(title->highID, title->lowID);
    if ((slotb >= 0) && promptConfirm(ST_YES_NO, gettext("Backup current savedata first?")))
        backupSavedata(title, slotb, allusers, common);
    uint32_t highID = title->highID;
    uint32_t lowID = title->lowID;
    bool isUSB = title->isTitleOnUSB;
    bool isWii = ((highID & 0xFFFFFFF0) == 0x00010000);
    char srcPath[PATH_SIZE];
    char origPath[PATH_SIZE];
    const char *path = (isWii ? "/vol/storage_slccmpt01/title" : (isUSB ? "/vol/storage_usb01/usr/save" : "/vol/storage_mlc01/usr/save"));
    sprintf(srcPath, "%s/%08x/%08x/%s", path, highID, lowID, isWii ? "data" : "user");
    if ((allusers > -1) && !isWii) {
        uint32_t offset = strlen(srcPath);
        if (common) {
            strcpy(srcPath + offset, "/common");
            sprintf(origPath, "%s", srcPath);
            if (!removeDir(srcPath))
                promptError(gettext("Common save not found."));
            if (unlink(origPath) == -1)
                promptError(gettext("Failed to delete common folder.\n%s"), strerror(errno));
        }
        sprintf(srcPath + offset, "/%s", wiiuacc[allusers].persistentID);
        sprintf(origPath, "%s", srcPath);
    }

    if (!removeDir(srcPath))
        promptError(gettext("Failed to delete savefile."));
    if ((allusers > -1) && !isWii) {
        if (unlink(origPath) == -1)
            promptError(gettext("Failed to delete user folder.\n%s"), strerror(errno));
    }
}

void importFromLoadiine(Title *title, bool common, int version) {
    if (!promptConfirm(ST_WARNING, gettext("Are you sure?")))
        return;
    int slotb = getEmptySlot(title->highID, title->lowID);
    if (slotb >= 0 && promptConfirm(ST_YES_NO, gettext("Backup current savedata first?")))
        backupSavedata(title, slotb, 0, common);
    uint32_t highID = title->highID;
    uint32_t lowID = title->lowID;
    bool isUSB = title->isTitleOnUSB;
    char srcPath[PATH_SIZE];
    char dstPath[PATH_SIZE];
    if (getLoadiineGameSaveDir(srcPath, title->productCode, title->longName, title->highID, title->lowID) != 0)
        return;
    if (version != 0)
        sprintf(srcPath + strlen(srcPath), "/v%i", version);
    const char *usrPath = {getUserID().c_str()};
    uint32_t srcOffset = strlen(srcPath);
    getLoadiineUserDir(srcPath, srcPath, usrPath);
    sprintf(dstPath, "%s/usr/save/%08x/%08x/user", isUSB ? "/vol/storage_usb01" : "/vol/storage_mlc01", highID, lowID);
    createFolder(dstPath);
    uint32_t dstOffset = strlen(dstPath);
    sprintf(dstPath + dstOffset, "/%s", usrPath);
    if (copyDir(srcPath, dstPath) != 0)
        promptError(gettext("Failed to import savedata from loadiine."));
    if (common) {
        strcpy(srcPath + srcOffset, "/c\0");
        strcpy(dstPath + dstOffset, "/common\0");
        if (copyDir(srcPath, dstPath) != 0)
            promptError(gettext("Common save not found."));
    }
}

void exportToLoadiine(Title *title, bool common, int version) {
    if (!promptConfirm(ST_WARNING, gettext("Are you sure?")))
        return;
    uint32_t highID = title->highID;
    uint32_t lowID = title->lowID;
    bool isUSB = title->isTitleOnUSB;
    char srcPath[PATH_SIZE];
    char dstPath[PATH_SIZE];
    if (getLoadiineGameSaveDir(dstPath, title->productCode, title->longName, title->highID, title->lowID) != 0)
        return;
    if (version != 0)
        sprintf(dstPath + strlen(dstPath), "/v%u", version);
    const char *usrPath = {getUserID().c_str()};
    uint32_t dstOffset = strlen(dstPath);
    getLoadiineUserDir(dstPath, dstPath, usrPath);
    sprintf(srcPath, "%s/usr/save/%08x/%08x/user", isUSB ? "/vol/storage_usb01" : "/vol/storage_mlc01", highID, lowID);
    uint32_t srcOffset = strlen(srcPath);
    sprintf(srcPath + srcOffset, "/%s", usrPath);
    createFolder(dstPath);
    if (copyDir(srcPath, dstPath) != 0)
        promptError(gettext("Failed to export savedata to loadiine."));
    if (common) {
        strcpy(dstPath + dstOffset, "/c\0");
        strcpy(srcPath + srcOffset, "/common\0");
        if (copyDir(srcPath, dstPath) != 0)
            promptError(gettext("Common save not found."));
    }
}
