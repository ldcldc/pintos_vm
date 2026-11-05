/* fork한 자식이 mmap(file-backed) 페이지를 COW로 공유하는지,
 * write 하면 자기 프레임으로 분리되는지 확인한다. */

#include <string.h>
#include <syscall.h>
#include "tests/lib.h"
#include "tests/main.h"
#include "tests/vm/sample.inc"

#define MAP_ADDR ((void *) 0x10000000)

void
test_main (void) {
  int handle;
  char *map;
  void *pa_parent;
  pid_t child;

  CHECK ((handle = open ("sample.txt")) > 1, "open \"sample.txt\"");
  CHECK ((map = mmap (MAP_ADDR, 4096, 1, handle, 0)) != MAP_FAILED, "mmap \"sample.txt\"");
  CHECK (!memcmp (map, sample, strlen (sample)), "read of mmap'd file");
  pa_parent = get_phys_addr (MAP_ADDR);

  /* (1) 자식이 아무것도 쓰지 않고 exit → 공유 프레임이 살아 있어야 함 */
  child = fork ("child");
  if (child == 0) {
      CHECK (get_phys_addr (MAP_ADDR) == pa_parent, "child shares parent's frame");
      return;
  }
  if (wait (child) != 0)
    fail ("case 1: child failed");
  CHECK (get_phys_addr (MAP_ADDR) == pa_parent, "parent keeps shared frame");

  /* (2) 부모가 write(dirty)한 뒤 fork → 자식은 같은 값, 같은 프레임을 본다 */
  map[0] = 'P';
  child = fork ("child");
  if (child == 0) {
      CHECK (map[0] == 'P', "child sees parent's write");
      CHECK (get_phys_addr (MAP_ADDR) == pa_parent, "child shares dirty frame");

      /* write 하는 순간 분리 */
      map[1] = 'K';
      CHECK (get_phys_addr (MAP_ADDR) != pa_parent, "child got private frame");
      CHECK (map[1] == 'K', "child sees own write");
      return;
  }
  if (wait (child) != 0)
    fail ("case 2: child failed");

  /* 부모 데이터는 자식의 write에 영향받지 않아야 함 */
  CHECK (map[0] == 'P', "parent keeps its own write");
  CHECK (map[1] == sample[1], "parent data unaffected by child");
  munmap (map);
}
