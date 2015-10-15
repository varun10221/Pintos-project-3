# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected (IGNORE_EXIT_CODES => 1, [<<'EOF']);
(mmap-simple) begin
(mmap-simple) open "sample.txt"
(mmap-simple) mmap "sample.txt"
(mmap-simple) mmap "sample.txt"
(mmap-simple) mmap "sample.txt"
(mmap-simple) mmap "sample.txt"
(mmap-simple) mmap "sample.txt"
(mmap-simple) mmap "sample.txt"
(mmap-simple) mmap "sample.txt"
(mmap-simple) mmap "sample.txt"
(mmap-simple) mmap "sample.txt"
(mmap-simple) mmap "sample.txt"
(mmap-simple) end
EOF
pass;
