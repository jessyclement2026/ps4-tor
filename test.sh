# 1. Create a .gitignore file to exclude the SDK and compiled files
cat << 'EOF' > .gitignore
ps4-payload-sdk/
*.elf
*.o
*.a
EOF

git init -b main

# 3. Stage your files (Git will automatically skip ps4-payload-sdk)
git add .

# 4. Double-check that ps4-payload-sdk is NOT listed
git status

# 5. Commit and push to GitHub
git commit -m "Initial commit: PS4 Tor daemon payload"
git remote add origin https://github.com/jessyclement2026/tor-ps4.git
git push -u origin main
