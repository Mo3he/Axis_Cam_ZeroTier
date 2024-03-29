# The ZeroTier installer ACAP

This ACAP packages the scripts and files required to install the ZeroTier VPN client on Axis Cameras.

Current version 1.12.2

**This repo is still under development, any and all ideas or assistance would be greatly appreciated.**

## Warning
Axis is making changes to its firmware that will include the removal of root privileges from ACAP.
With the release of Axis OS 12, ACAP's requiring root will no longer work.
The ZeroTier ACAP requires root to function.

You can read more here

https://help.axis.com/en-us/axis-os#upcoming-breaking-changes

If you have a use case where certain functionality used by an ACAP application currently requires root-user permissions or have a question about ACAP application signing, please contact Axis at acap-privileges@axis.com

Thank you for your continued support.

## Purpose

Adding a VPN client directly to the camera allows secure remote access to the device without requiring any other equipment or network configuration.
ZeroTier achieves this in a secure way.

## Links

https://zerotier.com/

https://github.com/zerotier 

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

cd to /usr/local/packages/ZeroTier_VPN/lib/ then use ./zerotier-one -q to interface with the cli.

Run 

```
./zerotier-one -q join <network id>
```
where `<network id>` is your network id to join a network.

When uninstalling the ACAP, all changes and files are removed from the camera.

You will need a ZeroTier.com account to use the ACAP

**please ignore the output from the app log, this is still being worked out, if you want to know the status run ./zerotier-one -q status**

## Building it yourself

The eap files will be updated from time to time and simply installing the new version over the old will update all files.

It's also possible to build and use a locally built image as all necesary files are provided.

Replace binarie "zerotier-one" in lib folder with new versions.
Make sure you use the files for the correct Soc.

Latest versions can be built at 

https://github.com/Mo3he/ZeroTierOne-Static


To build, 
From main directory of the version you want (arm/aarch64)

```
docker build --tag <package name> . 
```
```
docker cp $(docker create <package name>):/opt/app ./build 
```

