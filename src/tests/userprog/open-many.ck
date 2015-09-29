# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected ([<<'EOF']);
(open-many) begin
(open-many) end
open-many: exit(0)
EOF
pass;
