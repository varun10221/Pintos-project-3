# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected (IGNORE_USER_FAULTS => 1, IGNORE_EXIT_CODES => 1, [<<'EOF']);
(oom-nowait) begin
(oom-nowait) success. program created at least 20 children each iteration, and the same number were created each time.
(oom-nowait) end
EOF
pass;
