# TaskManagerBitmap
Takes a bitmap that is the size or width of Task Managers's CPU core view (ideally with single cells showing instantaneous percent CPU) and scrolls the bitmap using threads that consume CPU based on the pixel's greyscale (black is 100% CPU). 

![TaskManagerBitmap showing Azure Logo on 420 core system](https://github.com/markrussinovich/TaskManagerBitmap/blob/main/AzureBeast.gif?raw=true)

## Setup

- Install the latest version of VS on godzilla machine
- Clone repo on godzilla machine
- build TaskManagerBitmap.sln
- cd to the compiled directory
- copy your bitmap to the compiled directory
- ./taskmanagerBitMpa.exe ./[bitmapFileName] [width of bitmap]
- open task manager, click on performance, right click graph and switch to logical processors view
- resize task manager window until you get the right "pixel" size for your bitmap and the painted bitmap looks correct in the task manager