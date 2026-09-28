# pin-routing test fixtures

The full-size netlist + pin map that used to live here (`example_board.v` /
`example_board.ve`) came from the vendor reference design and is **not
redistributed** with this repository. Drop a copy with those two names into
this directory to re-enable the three cases in
`tools/tests/test_pin_routing_tools.py::TestRealNetlistFixture`; without it
they skip.

What those cases cover (and what a future synthetic fixture has to reproduce):
the netlist parser against real formatting (multi-line assigns, the
`gpio*_io_in` concatenations, CPLD signals spelled instead of pins), the two
pins the pin map claims but the wrapper never wires, and `check_pinctrl.py`
exit code 7 for the SPI0 pin list whose Case C signals resolve while CSN/SCK
do not.
