//Make Your own Distro by #include-ing this file.

#define STD_DISTRO_DVD_CONFIG 	"TB\nScale2Mem(2048,0x40000)\nT \n\n1024\n768\n\n\n"

U0 MakeMyISO(U8 *_out_iso_filename)
{//Does everything with current drive.
//If you have not recompiled $FG,2$Kernel$FG$ and defined your CD/DVD drive, use $LK,"Mount",A="MN:Mount"$.
	U8 *out_iso_filename = FileNameAbs(_out_iso_filename);
	if (!DriveIsWritable)
	{
		"Drive must be writable.	Install on Hard drive, first.\n";
		return;
	}
	DelTree("/Distro");
	Del(out_iso_filename);

	DirMake("/Distro");
	In(STD_DISTRO_DVD_CONFIG);
	BootDVDIns;

	Copy("/*", "/Distro");
	Del("/Distro/" KERNEL_BIN_C);

	CopyTree(BOOT_DIR,		"/Distro" BOOT_DIR);
	CopyTree("/Home", 		"/Distro/Home");
	Del("/Distro/Home/Registry.CC");
	CopyTree("/Zenith", 	"/Distro/Zenith");
	CopyTree("/Apps", 		"/Distro/Apps");
	CopyTree("/Compiler",	"/Distro/Compiler");
	CopyTree("/Demo", 		"/Distro/Demo");
	CopyTree("/Doc",		"/Distro/Doc");
	CopyTree("/Kernel", 	"/Distro/Kernel");
	CopyTree("/Misc", 		"/Distro/Misc");

	//To save space, optionally delete dictionary.
	//Del("/Distro/Zenith/AutoComplete/ACDefs.DATA");
	CopyTree("/Downloads", "/Distro/Downloads"); 		//You can leave this out.
	DirMake("/Distro/Tmp");
	DirMake("/Distro/Tmp/ScreenShots");
	RedSeaISO(out_iso_filename, "/Distro", "/Distro" BOOT_DIR_KERNEL_BIN_C);

	//If CD-ROM use MT_CD instead of MT_DVD.
	//DVDImageWrite('T',out_iso_filename,MT_DVD); //Uncomment to burn.

	DelTree("/Distro");
	Free(out_iso_filename);
}

MakeMyISO("/Tmp/MyDistro.ISO.C");

// Study my account examples $LK,"Config Strs",A="FL:::/Demo/AcctExample/TOS/TOSConfig.CC,1"$, $LK,"Update Funs",A="FL:::/Demo/AcctExample/TOS/TOSDistro.CC,1"$.
