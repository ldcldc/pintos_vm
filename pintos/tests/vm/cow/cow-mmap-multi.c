/* 여러 페이지를 mmap한 뒤 fork 했을 때, 한 페이지만 write 해도
 * 그 페이지만 분리되고 나머지 페이지는 계속 공유되는지 확인한다. */

#include <string.h>
#include <syscall.h>
#include "tests/lib.h"
#include "tests/main.h"

#define MAP_ADDR ((void *) 0x10000000)
#define NPAGES 3
#define MAP_SIZE (NPAGES * 4096)

static char pattern[MAP_SIZE];

void
test_main (void) {
  int handle;
  char *map;
  void *pa[NPAGES];
  size_t i;
  pid_t child;

  for (i = 0; i < MAP_SIZE; i++)
    pattern[i] = (char) i;

  CHECK (create ("cowbig", MAP_SIZE), "create \"cowbig\"");
  CHECK ((handle = open ("cowbig")) > 1, "open \"cowbig\"");
  CHECK (write (handle, pattern, MAP_SIZE) == (int) MAP_SIZE, "write \"cowbig\"");

  CHECK ((map = mmap (MAP_ADDR, MAP_SIZE, 1, handle, 0)) != MAP_FAILED, "mmap \"cowbig\"");

  /* 전체 페이지를 물리 메모리에 올리고 각 페이지의 물리 주소를 기록 */
  for (i = 0; i < NPAGES; i++) {
      CHECK (map[i * 4096] == pattern[i * 4096], "read page %zu", i);
      pa[i] = get_phys_addr (map + i * 4096);
  }

  child = fork ("child");
  if (child == 0) {
      CHECK (get_phys_addr (map + 4096) == pa[1], "child shares page 1");

      map[4096] = 'X';
      CHECK (get_phys_addr (map + 4096) != pa[1], "page 1 splitted");
      CHECK (map[4096] == 'X', "child sees own write");

      /* 나머지 페이지는 여전히 공유 상태여야 함 */
      CHECK (get_phys_addr (map) == pa[0], "page 0 still shared");
      CHECK (get_phys_addr (map + 2 * 4096) == pa[2], "page 2 still shared");
      return;
  }
  if (wait (child) != 0)
    fail ("child failed");

  CHECK (get_phys_addr (map + 4096) == pa[1], "parent page 1 unchanged");
  CHECK (map[4096] == pattern[4096], "parent data unaffected");
  munmap (map);
}
