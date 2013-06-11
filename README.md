nsrepl
======

#### Summary ####
[AOLserver](http://code.google.com/p/aolserver/) Read-Eval-Print-Loop accessible via a Unix Domain Socket.  Code adapted from [nscp](http://code.google.com/p/aolserver/wiki/nscp).

#### Introduction ####
nsrepl is a module that allows you to control and run arbitrary code inside a running AOLserver's address space.  This means you can inspect a running interpreter to get the values of global variables, list the names and bodies of procedures, examine namespaces, and test snippets of code.  This can provide a tighter edit-debug cycle that does not require a lot of switching from one window to another, browser reloads, and intermediate software layers.  This can also provide better integration with external editors that are capable of driving command interpreters, such as emacs' `tcl-mode'.

#### Configuration ####
All module configuration parameters are optional.  A complete module configuration looks like:

    ns_section  "ns/server/$server/module/nsrepl"
    ns_param    listenAtPath    "$server_root/var/run/repl"
    ns_param    logCommandsP    true
    
    ns_section  "ns/server/$server/modules"
    ns_param    nsrepl          ${aolserver_bindir}/nsrepl.so
    

#### Connecting ####
Any tool capable of reading and writing to a UNIX domain socket can access the REPL.  For example, [socat](http://www.dest-unreach.org/socat/):

    socat STDIO var/run/repl

#### Limitations ####
nsrepl uses UNIX domain sockets for its communication.  This means that connections can only be opened on the machine on which the AOLserver process is running and only if the user accessing the UNIX domain socket has been granted read and write privileges on the socket's inode.
