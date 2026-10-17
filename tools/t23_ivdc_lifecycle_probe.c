/* SPDX-License-Identifier: MIT
 * Native OEM80 lifecycle probe. Requires externally arranged media/watchdog
 * ownership. Does not alter tuning, module parameters, boot files or power. */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "imp/imp_isp.h"
#include "imp/imp_framesource.h"

static int owners(void)
{
    DIR *proc = opendir("/proc");
    if (!proc) return -1;
    struct dirent *e;
    while ((e = readdir(proc))) {
        char *end; long pid = strtol(e->d_name, &end, 10);
        if (*end || pid < 1 || pid == (long)getpid()) continue;
        char path[256], text[512] = {0};
        snprintf(path, sizeof path, "/proc/%ld/comm", pid);
        FILE *f = fopen(path, "r");
        if (f) { (void)fgets(text, sizeof text, f); fclose(f); }
        if (!strncmp(text, "jooanipc", 8)) { closedir(proc); errno = EBUSY; return -1; }
        snprintf(path, sizeof path, "/proc/%ld/fd", pid);
        DIR *fds = opendir(path);
        if (!fds) { if (errno == EACCES || errno == EPERM) { closedir(proc); return -1; } continue; }
        struct dirent *fd;
        while ((fd = readdir(fds))) {
            if (*fd->d_name == '.') continue;
            char link[512];
            snprintf(link, sizeof link, "/proc/%ld/fd/%s", pid, fd->d_name);
            ssize_t n = readlink(link, text, sizeof text - 1);
            if (n < 0) continue;
            text[n] = 0;
            const char *names[] = {"/dev/rmem", "/dev/tx-isp", "/dev/isp-m", "/dev/framechan", "/dev/misc-ivdc", "/dev/soc_vpu"};
            for (unsigned i = 0; i < sizeof names / sizeof names[0]; ++i)
                if (!strncmp(text, names[i], strlen(names[i]))) { closedir(fds); closedir(proc); errno = EBUSY; return -1; }
        }
        closedir(fds);
    }
    closedir(proc); return 0;
}
static int kernel_taint(unsigned long *value)
{
    FILE *file = fopen("/proc/sys/kernel/tainted", "r");
    if (!file) return -1;
    int result = fscanf(file, "%lu", value) == 1 ? 0 : -1;
    if (fclose(file) != 0) result = -1;
    return result;
}
static void hold(void)
{
    fputs("UNSAFE: cleanup retained resources; verify detach or reset camera. Holding process/library mapping.\n", stderr);
    signal(SIGHUP, SIG_IGN); signal(SIGTERM, SIG_IGN); signal(SIGINT, SIG_IGN);
    for (;;) pause();
}
#define LOAD(field, name) do { void *symbol = dlsym(lib, name); memcpy(&field, &symbol, sizeof symbol); if (!symbol) { fprintf(stderr,"missing %s\n",name); return 2; } } while (0)
#define STEP(call, name) do { fprintf(stderr,"begin %s\n",name); int rc = (call); if (rc) { fprintf(stderr,"FAIL %s rc=%d errno=%d\n",name,rc,errno); failed=1; goto cleanup; } } while (0)
#define UNDO(call, name) do { fprintf(stderr,"cleanup %s\n",name); if ((call) != 0) hold(); } while (0)
int main(int argc, char **argv)
{
#if !defined(__mips__) || __SIZEOF_POINTER__ != 4 || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
    (void)argc; (void)argv; fputs("Requires native MIPS32/o32 little endian\n",stderr); return 2;
#else
    setvbuf(stderr,NULL,_IONBF,0);
    if (argc != 3 || strcmp(argv[1],"--run") || argv[2][0] != '/') { fprintf(stderr,"Usage: %s --run /absolute/path/libimp.so\n",argv[0]); return 2; }
    const char *abi=getenv("OPENIMP_T23_DRIVER_ABI"), *enc=getenv("OPENIMP_T23_ENCODER"), *fallback=getenv("OPENIMP_T23_NATIVE_FALLBACK");
    if (!abi || strcmp(abi,"oem80") || !enc || strcmp(enc,"native") || !fallback || strcmp(fallback,"0")) { fputs("Explicit oem80/native/no-fallback environment required\n",stderr); return 2; }
    if (owners()!=0) { perror("exclusive media ownership required"); return 2; }
    unsigned long before_taint, after_taint;
    /* Linux TAINT_DIE is bit 7. Do not probe an already damaged kernel. */
    if (kernel_taint(&before_taint) != 0 || (before_taint & (1ul << 7))) {
        fputs("Kernel health prerequisite failed; reset before probing\n",stderr); return 2;
    }
    void *lib=dlopen(argv[2],RTLD_NOW|RTLD_LOCAL);
    if (!lib) { fprintf(stderr,"dlopen: %s\n",dlerror()); return 2; }
    int (*version)(char *), (*isp_open)(void), (*isp_close)(void), (*enable)(void), (*disable)(void), (*init)(void), (*exit_sys)(void);
    int (*add)(IMPSensorInfo *), (*del)(IMPSensorInfo *);
    int (*create)(int,IMPFSChnAttr *), (*set)(int,IMPFSChnAttr *), (*start)(int), (*stop)(int), (*destroy)(int);
    LOAD(version,"IMP_System_GetVersion"); LOAD(isp_open,"IMP_ISP_Open"); LOAD(isp_close,"IMP_ISP_Close");
    LOAD(enable,"IMP_ISP_EnableSensor"); LOAD(disable,"IMP_ISP_DisableSensor"); LOAD(add,"IMP_ISP_AddSensor"); LOAD(del,"IMP_ISP_DelSensor");
    LOAD(init,"IMP_System_Init"); LOAD(exit_sys,"IMP_System_Exit"); LOAD(create,"IMP_FrameSource_CreateChn"); LOAD(set,"IMP_FrameSource_SetChnAttr");
    LOAD(start,"IMP_FrameSource_EnableChn"); LOAD(stop,"IMP_FrameSource_DisableChn"); LOAD(destroy,"IMP_FrameSource_DestroyChn");
    char v[128]={0}; if (version(v)!=0 || strcmp(v,"libimp.so T23 openimp")) return 2;
    int failed=0,opened=0,added[2]={0},enabled=0,system=0,created=0,started=0;
    IMPSensorInfo sensor[2]; memset(sensor,0,sizeof sensor);
    STEP(isp_open(),"ISP_Open");opened=1;
    for (unsigned i=0;i<2;++i) {
        snprintf(sensor[i].name,sizeof sensor[i].name,"%s",i?"cv2005s1":"cv2005");
        snprintf(sensor[i].i2c.type,sizeof sensor[i].i2c.type,"%s",sensor[i].name);
        sensor[i].reserved1=(int)i; sensor[i].cbus_type=TX_SENSOR_CONTROL_INTERFACE_I2C;
        sensor[i].i2c.addr=i?0x36:0x35; sensor[i].i2c_adapter=0;
        STEP(add(&sensor[i]),"AddSensor"); added[i]=1;
    }
    enabled=1; /* A failed enable can still have started the global ISP. */
    STEP(enable(),"EnableSensor");
    STEP(init(),"System_Init");system=1;
    IMPFSChnAttr attr={0};attr.picWidth=640;attr.picHeight=360;attr.pixFmt=PIX_FMT_NV12;
    attr.crop.enable=1;attr.crop.width=640;attr.crop.height=360;
    attr.scaler.enable=1;attr.scaler.outwidth=640;attr.scaler.outheight=360;
    attr.outFrmRateNum=15;attr.outFrmRateDen=1;attr.nrVBs=1;attr.type=FS_PHY_CHANNEL;
    STEP(create(0,&attr),"CreateChn0");created=1;
    STEP(set(0,&attr),"SetChnAttr0");
    STEP(start(0),"EnableChn0");started=1;
    usleep(200000);
cleanup:
    if(started) UNDO(stop(0),"DisableChn0");
    if(created) UNDO(destroy(0),"DestroyChn0");
    if(system) UNDO(exit_sys(),"System_Exit");
    if(enabled) UNDO(disable(),"DisableSensor");
    for(int i=1;i>=0;--i) if(added[i]) UNDO(del(&sensor[i]),"DelSensor");
    if(opened) UNDO(isp_close(),"ISP_Close");
    dlclose(lib);
    if (kernel_taint(&after_taint) != 0 || after_taint != before_taint) {
        fputs("FAIL: kernel taint changed or health check failed; inspect kernel log\n",stderr); return 4;
    }
    fputs(failed?"FAIL: setup failed; cleanup completed\n":"PASS: native IVDC lifecycle completed\n",stderr);
    return failed?1:0;
#endif
}
