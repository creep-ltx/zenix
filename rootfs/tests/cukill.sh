# cu-test (audit A10): cu killed from outside; its receiving half (cu -r) ends at the next byte from the other
# computer and gives the port back, so the last line is on the serial port again
cu > /tmp/cu2.out &
sleep 3
kill $!
sleep 6
echo cukill-$(echo done)
