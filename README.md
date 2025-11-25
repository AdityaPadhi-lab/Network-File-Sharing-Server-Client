# 🗂️ Network File Sharing Server & Client

A client-server application for sharing files over the network with user authentication, directory navigation, upload/download support and progress tracking.

---

## 🚀 Features
- User authentication (login/register)  
- File upload, download, delete operations  
- Directory navigation (list folders/files)  
- Progress tracking for large file transfers  
- Client & server modules separated  
- Clear network protocol and error handling  

---

## 🧠 Project Workflow (How the System Works)
### 1️⃣ Client Input & Authentication
- Client connects to server (IP + port).  
- User logs in or registers a new account.  
- Authentication validated by server.

### 2️⃣ Directory Listing & Navigation
- After login, client requests server to list directories/files.  
- Client can navigate into sub-folders or go back.

### 3️⃣ File Operations
- **Upload**: Client sends file metadata → server receives and writes file → server confirms.  
- **Download**: Client requests file → server streams file → client writes locally with progress.  
- **Delete**: Client requests deletion → server verifies permissions → file removed.

### 4️⃣ Progress Tracking & Error Handling
- Both client & server display progress for large files.  
- Errors (network drop, permissions, file not found) logged and handled gracefully.

### 5️⃣ Modular Architecture
