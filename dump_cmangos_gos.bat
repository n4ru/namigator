@echo off
rem This is just intended to be an example.  You will have to adjust the outfile parameter and database name to match your environment
echo SELECT guid,gameobject_template.displayId,map,position_x,position_y,position_z,rotation0,rotation1,rotation2,rotation3 FROM gameobject,gameobject_template WHERE gameobject.id = gameobject_template.entry AND gameobject_template.type != 0 into outfile 'c:/ProgramData/MySQL/MySQL Server 5.7/Uploads/gos.csv' fields terminated by ',' enclosed by '' lines terminated by '\n'; |mysql -u root -p classic_world
copy "c:/ProgramData/MySQL/MySQL Server 5.7/Uploads/gos.csv" .