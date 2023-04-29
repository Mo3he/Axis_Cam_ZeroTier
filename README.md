# The ZeroTier installer ACAP

This ACAP packages the scripts and files required to install the ZeroTier VPN client on Axis Cameras.

Current version 1.00.0

## Purpose

Adding a VPN client directly to the camera allows secure remote access to the device without requiring any other equipment or network configuration.
ZeroTier achieves this in a secure way.
ZeroTier is based on WireGuard VPN tunneling technology.

## Links

https://ZeroTier.com/

https://github.com/ZeroTier 

https://www.wireguard.com/

https://www.axis.com/

## Compatibility

The ZeroTier ACAP is compatable with Axis cameras with arm and aarch64 based Soc's.

```
curl --anyauth "*" -u <username>:<password> <device ip>/axis-cgi/basicdeviceinfo.cgi --data "{\"apiVersion\":\"1.0\",\"context\":\"Client defined request ID\",\"method\":\"getAllProperties\"}"
```

where `<device ip>` is the IP address of the Axis device, `<username>` is the root username and `<password>` is the root password. Please
note that you need to enclose your password with quotes (`'`) if it contains special characters.

## Installing

The recommended way to install this ACAP is to use the pre built eap file.
Go to "Apps" on the camera and click "Add app".


## Using the ZeroTier ACAP

Once running you will need to enable ssh on the camera then connect via ssh to control ZeroTier via the cli.
you will need to cd to /usr/local/packages/ZeroTier_VPN/lib/ then use ./zerotier-one -q to interface with the cli for example ./zerotier-one -d -p(portnumber) to change the default port and run as dameon or ./zerotier-one -q join (network id) to join a network.
This project is in its really early stagus so any and all help would be welcome.

When uninstalling the ACAP, all changes and files are removed from the camera.

You will need a ZeroTier.com account to use the ACAP

## Updating ZeroTier version

The eap files will be updated from time to time and simply installing the new version over the old will update all files.

It's also possible to build and use a locally built image as all necesary files are provided.

Replace binarie "zerotier-one" in lib folder with new versions. (remove the .aarch64 or .arm extension from the file name)
Make sure you use the files for the correct Soc.

Latest versions can be found at 

https://download.zerotier.com/RELEASES/1.4.6/dist/static-binaries/


To build, 
From main directory of the version you want (arm/aarch64)

```
docker build --tag <package name> . 
```
```
docker cp $(docker create <package name>):/opt/app ./build 
```

