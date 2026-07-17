#include <stdint.h>
#include "git_sha.h"

#ifndef BM_SBC_APP_NAME
#define BM_SBC_APP_NAME "bm_sbc"
#endif

#define MAX_VERSION_STR_LEN 96

// Binary identity data baked in at build time.
// DFU validation and the DFU host scanner both search for these in the binary.
#ifdef __linux__
__attribute__((used, section(".rodata")))
#else
__attribute__((used))
#endif
static const char k_image_marker[] = "BM_SBC_IMAGE:" BM_SBC_APP_NAME;


// This version info data is currently not used by the bm_sbc apps.
// However, it is included specifically for bm dfu processes that will
// scan the binary for this magic number to then extract the version
// info struct. This struct is then used to populate the start
// messages for the bm dfu process, allowing for a cancellation of
// the update if the version or gitsha already match
// (assuming the force flag is not used).
#define VERSION_INFO_MAGIC UINT64_C(0xDF7F9AFDEC06627C)

typedef struct {
  uint64_t magic;           // Magic number to identify this as a valid versionInfo_t
  uint32_t gitSHA;          // git SHA for image
  uint8_t maj;              // Major version
  uint8_t min;              // Minor version
  uint8_t rev;              // revision
  uint8_t hwVersion;        // Hardware version (0 for don't care)
  uint32_t flags;           // Various flags (specified above)
  uint16_t versionStrLen;   // Version string length
  const char versionStr[MAX_VERSION_STR_LEN]; // Version string
} __attribute__((packed)) versionInfo_t;

#ifdef __linux__
__attribute__((used, section(".rodata")))
#else
__attribute__((used))
#endif
static const versionInfo_t k_version_info = {
  .magic         = VERSION_INFO_MAGIC,
  .gitSHA        = BM_SBC_GIT_SHA,
  .maj           = BM_SBC_VERSION_MAJOR,
  .min           = BM_SBC_VERSION_MINOR,
  .rev           = BM_SBC_VERSION_PATCH,
  .hwVersion     = 0,
  .flags         = 0,
  .versionStrLen = 0,  // Not used for now
  .versionStr    = "", // Not used for now
};
