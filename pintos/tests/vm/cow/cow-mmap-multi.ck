# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected (IGNORE_EXIT_CODES => 1, [<<'EOF']);
(cow-mmap-multi) begin
(cow-mmap-multi) create "cowbig"
(cow-mmap-multi) open "cowbig"
(cow-mmap-multi) write "cowbig"
(cow-mmap-multi) mmap "cowbig"
(cow-mmap-multi) read page 0
(cow-mmap-multi) read page 1
(cow-mmap-multi) read page 2
(cow-mmap-multi) child shares page 1
(cow-mmap-multi) page 1 splitted
(cow-mmap-multi) child sees own write
(cow-mmap-multi) page 0 still shared
(cow-mmap-multi) page 2 still shared
(cow-mmap-multi) end
(cow-mmap-multi) parent page 1 unchanged
(cow-mmap-multi) parent data unaffected
(cow-mmap-multi) end
EOF
pass;
