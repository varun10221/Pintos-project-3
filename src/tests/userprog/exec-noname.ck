# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected ([<<'EOF']);
(exec-noname) begin
(exec-noname) end
exec-noname: exit(0)
EOF
pass;
