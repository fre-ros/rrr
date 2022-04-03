from rrr_helper import *
import time
import termios

def config (config : rrr_config):
	print ("Python3 in config custom setting is " + config.get("python3_custom_setting"));
	return True

def process(socket : rrr_socket, message: rrr_message):
	print ("Python3 got a message, forwarding")

	array = message.get_array()
	value = array.get("str")
	print("Before" + str(value.get(0)) + "\n")
	value.set(0, value.get(0).decode("iso-8859-1"))
	print("After " + str(value.get(0)) + "\n")
	#value.set_type(11)

	# Produces crash
	#array.remove("str")
	#value_new = rrr_array_value()
	#value_new.set(0, str(value.get(0).decode("iso-8859-1")))
	#value_new.set_tag("str")
	#array.append(value_new)

	#Produces size mismatch
	#value = array.get("str")
	#value.set_type(11) -- SIZE MISMATCH DUE TO BYTE/CHAR COUNT MISMATCH
	#value.set(0, str(value.get(0).decode("iso-8859-1")))

	socket.send(message)

	return True

def source(socket : rrr_socket, message: rrr_message):
	print ("Python3 sourcing a message")

	my_array_value = rrr_array_value()
	my_array_value.set_tag("my_tag")
	my_array_value.set(0, "my_value")

	my_array = rrr_array()
	my_array.append(my_array_value)

	message.set_array(my_array)

	socket.send(message)

	time.sleep(1)

	return True
