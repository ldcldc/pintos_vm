# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected (IGNORE_EXIT_CODES => 1, [<<'EOF']);
(cow-mmap) begin
(cow-mmap) open "sample.txt"
(cow-mmap) mmap "sample.txt"
(cow-mmap) read of mmap'd file
(cow-mmap) child shares parent's frame
(cow-mmap) end
(cow-mmap) parent keeps shared frame
(cow-mmap) child sees parent's write
(cow-mmap) child shares dirty frame
(cow-mmap) child got private frame
(cow-mmap) child sees own write
(cow-mmap) end
(cow-mmap) parent keeps its own write
(cow-mmap) parent data unaffected by child
(cow-mmap) end
EOF
pass;
