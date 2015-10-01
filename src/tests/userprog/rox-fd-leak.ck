# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected ([<<'EOF']);
(rox-fd-leak) begin
(rox-fd-leak) open "rox-fd-leak"
(rox-fd-leak) read "rox-fd-leak"
(rox-fd-leak) try to write "rox-fd-leak"
(rox-fd-leak) end
rox-fd-leak: exit(0)
EOF
pass;
