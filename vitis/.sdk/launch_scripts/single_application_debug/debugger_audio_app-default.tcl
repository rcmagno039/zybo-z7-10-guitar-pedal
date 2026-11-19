connect -url tcp:127.0.0.1:3121
targets -set -nocase -filter {name =~"APU*"}
rst -system
after 3000
targets -set -filter {jtag_cable_name =~ "Digilent Zybo Z7 210351B80804A" && level==0} -index 1
fpga -file C:/Users/benteslp/Documents/PSN50/vitis/audio_app/_ide/bitstream/system_wrapper_v1.bit
targets -set -nocase -filter {name =~"APU*"}
loadhw -hw C:/Users/benteslp/Documents/PSN50/vitis/system_wrapper_v1/export/system_wrapper_v1/hw/system_wrapper_v1.xsa -mem-ranges [list {0x40000000 0xbfffffff}]
configparams force-mem-access 1
targets -set -nocase -filter {name =~"APU*"}
source C:/Users/benteslp/Documents/PSN50/vitis/audio_app/_ide/psinit/ps7_init.tcl
ps7_init
ps7_post_config
targets -set -nocase -filter {name =~ "*A9*#0"}
dow C:/Users/benteslp/Documents/PSN50/vitis/audio_app/Debug/audio_app.elf
configparams force-mem-access 0
targets -set -nocase -filter {name =~ "*A9*#0"}
con
