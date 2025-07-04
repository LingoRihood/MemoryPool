在项目目录下创建build目录，并进入该目录
```
mkdir build && cd build
```
执行 cmake 命令
```
cmake ..
```
执行 make 命令
```
make
```
删除编译生成的可执行文件：
```
make clean
```
运行
```
./MemoryPoolTest
```

# MemoryPoolC11 实验结果
![alt text](image.png)



上传到github指令
```
echo "# MemoryPool" >> README.md
git init
touch README.md
git add .
git commit -m "first commit"
git branch -M main
git remote add origin git@github.com:LingoRihood/MemoryPool.git
git push -u origin main
```

另附生成ssh key的官方文档链接
https://docs.github.com/en/authentication/connecting-to-github-with-ssh/generating-a-new-ssh-key-and-adding-it-to-the-ssh-agent?platform=linux

如果添加修改文件或二次上传，需要
- git add .
- git commit -m "Added new image"
- git push origin main