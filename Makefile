PROGRAM=aws_seminar

EXTRA_CFLAGS=-DLWIP_HTTPD_CGI=1 -DLWIP_HTTPD_SSI=1 -I./fsdata

EXTRA_COMPONENTS = extras/paho_mqtt_c extras/mbedtls extras/i2c extras/bmp280 extras/httpd

include ../../common.mk

html:
	@echo "Generating fsdata.."
	cd fsdata && ./makefsdata
