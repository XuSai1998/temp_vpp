.. _add_plugin:

Adding a plugin
===============

.. toctree::

Strategic Choices
_________________

Plugins may implement lightly-used, experimental, or test
functionality. In such cases, please disable the plugin by default:

.. code-block:: console

    /* *INDENT-OFF* */
    VLIB_PLUGIN_REGISTER () =
    {
      .version = VPP_BUILD_VER,
      .description = "Plugin Disabled by Default...",
      .default_disabled = 1,
    };
    /* *INDENT-ON* */

Please do not create processes, or other dynamic data structures
unless the plugin is configured by API or debug CLI.

Specifically, please don't initialize bihash tables from
VLIB_INIT_FUNCTIONS, *especially* if the bihash template involved
doesn't #define BIHASH_LAZY_INSTANTIATE 1.

.. code-block:: console

    static clib_error_t * sample_init (vlib_main_t * vm)
    {
      <snip>
      /* DONT DO THIS! */
      BV(clib_bihash_init (h, ...))
      <snip>
    }
    VLIB_INIT_FUNCTION (sample_init);

Instead, please add a feature_init function:

.. code-block:: console

    static void
    feature_init (my_main_t * mm)
    {
      if (mm->feature_initialized == 0)
        {
          BV(clib_bihash_init)(mm->hash_table, ...)
          /* Create Other Things, e.g a periodic process */
          mm->feature_initialized = 1;
        }
    }

And call it from debug CLI and API message handlers any time the feature
is enabled.

How to create a new plugin
__________________________

This section shows how a VPP developer can create a new plugin, and
add it to VPP. We assume that we are starting from the VPP <top-of-workspace>.

As an example, we will use the **make-plugin.sh** tool found in
**./extras/emacs**. make-plugin.sh is a simple wrapper for a comprehensive
plugin generator constructed from a set of emacs-lisp skeletons.

Change directory to **./src/plugins**, and run the plugin generator:

.. code-block:: console

    $ cd ./src/plugins
    $ ../../extras/emacs/make-plugin.sh
    <snip>
    Loading /scratch/vpp-docs/extras/emacs/tunnel-c-skel.el (source)...
    Loading /scratch/vpp-docs/extras/emacs/tunnel-decap-skel.el (source)...
    Loading /scratch/vpp-docs/extras/emacs/tunnel-encap-skel.el (source)...
    Loading /scratch/vpp-docs/extras/emacs/tunnel-h-skel.el (source)...
    Loading /scratch/vpp-docs/extras/emacs/elog-4-int-skel.el (source)...
    Loading /scratch/vpp-docs/extras/emacs/elog-4-int-track-skel.el (source)...
    Loading /scratch/vpp-docs/extras/emacs/elog-enum-skel.el (source)...
    Loading /scratch/vpp-docs/extras/emacs/elog-one-datum-skel.el (source)...
    Plugin name: myplugin
    Dispatch type [dual or qs]: dual
    (Shell command succeeded with no output)

    OK...

The plugin generator script asks two questions: the name of the
plugin, and which of two dispatch types to use. Since the plugin name
finds its way into quite a number of places - filenames, typedef
names, graph arc names - it pays to think for a moment.

The dispatch type refers to the coding pattern used to construct
**node.c**, the *pro forma* data-plane node. The **dual** option
constructs a dual-single loop pair with speculative enqueueing. This
is the traditional coding pattern for load-store intensive graph
nodes.

The **qs** option generates a quad-single loop pair which uses
vlib_get_buffers(...) and vlib_buffer_enqueue_to_next(...). These
operators make excellent use of available SIMD vector unit
operations. It's very simple to change a quad-single loop-pair to a
dual-single loop pair if you decide to do so later.

Generated Files
---------------

Here are the generated files. We'll go through them in a moment.

.. code-block:: console

    $ cd ./myplugin
    $ ls
    CMakeLists.txt  myplugin.api  myplugin.c  myplugin.h
    myplugin_periodic.c  myplugin_test.c  node.c  setup.pg

Due to recent build system improvements, you **don't** need to touch
any other files to integrate your new plugin into the vpp build. Simply
rebuild your workspace from scratch, and the new plugin will appear.

Rebuild your workspace
----------------------

This is the straightforward way to reconfigure and rebuild your workspace:

.. code-block:: console

    $ cd <top-of-workspace>
    $ make rebuild [or rebuild-release]

Thanks to ccache, this operation doesn't take an annoying amount of time.

Sanity check: run vpp
---------------------

As a quick sanity check, run vpp and make sure that
"myplugin_plugin.so" and "myplugin_test_plugin.so" are loaded:

.. code-block:: console

    $ cd <top-of-workspace>
    $ make run
    <snip>
    load_one_plugin:189: Loaded plugin: myplugin_plugin.so (myplugin description goes here)
    <snip>
    load_one_vat_plugin:67: Loaded plugin: myplugin_test_plugin.so
    <snip>
    DBGvpp#

If this simple test fails, please seek assistance.

Generated Files in Detail
_________________________

This section discusses the generated files in some detail. It's fine to
skim this section, and return later for more detail.

CMakeLists.txt
--------------

This is the build system recipe for building your plugin. Please fix
the copyright notice:

.. code-block:: console

    # Copyright (c) <current-year> <your-organization>

The rest of the build recipe is pretty simple:

.. code-block:: CMake

    add_vpp_plugin (myplugin
    SOURCES
    myplugin.c
    node.c
    myplugin_periodic.c
    myplugin.h

    MULTIARCH_SOURCES
    node.c

    API_FILES
    myplugin.api

    API_TEST_SOURCES
    myplugin_test.c
    )

As you can see, the build recipe consists of several lists of
files. **SOURCES** is a list of C source files. **API_FILES** is a
list of the plugin's binary API definition files [one such file is
usually plenty], and so forth.

**MULTIARCH_SOURCES** lists data plane graph node dispatch function
source files considered to be performance-critical. Specific functions
in these files are compiled multiple times, so that they can leverage
CPU-specific features. More on this in a moment.

If you add source files, simply add them to the indicated list(s).

myplugin.h
----------

This is the primary #include file for the new plugin. Among other
things, it defines the plugin's *main_t* data structure. This is the
right place to add problem-specific data structures. Please **resist
the temptation** to create a set of static or [worse yet] global
variables in your plugin. Refereeing name-collisions between plugins
is not anyone's idea of a good time.

myplugin.c
----------

For want of a better way to describe it, myplugin.c is the vpp plugin
equivalent of "main.c". Its job is to hook the plugin into the vpp
binary API message dispatcher, and to add its messages to vpp's global
"message-name_crc" hash table. See "myplugin_init (...")"

Vpp itself uses dlsym(...) to track down the vlib_plugin_registration_t
generated by the VLIB_PLUGIN_REGISTER macro:

.. code-block:: C

    VLIB_PLUGIN_REGISTER () =
      {
        .version = VPP_BUILD_VER,
        .description = "myplugin plugin description goes here",
      };

Vpp only loads .so files from the plugin directory which contain an
instance of this data structure.

You can enable or disable specific vpp plugins from the command
line. By default, plugins are loaded. To change that behavior, set
default_disabled in the macro VLIB_PLUGIN_REGISTER:

.. code-block:: C

    VLIB_PLUGIN_REGISTER () =
      {
        .version = VPP_BUILD_VER,
        .default_disabled = 1
        .description = "myplugin plugin description goes here",
      };

The boilerplate generator places the graph node dispatch function
onto the "device-input" feature arc. This may or may not be useful.

.. code-block:: C

    VNET_FEATURE_INIT (myplugin, static) =
    {
      .arc_name = "device-input",
      .node_name = "myplugin",
      .runs_before = VNET_FEATURES ("ethernet-input"),
    };

As given by the plugin generator, myplugin.c contains the binary API
message handler for a generic "please enable my feature on such and
such an interface" binary API message. As you'll see, setting up the
vpp message API tables is simple. Big fat warning: the scheme is
intolerant of minor mistakes. Example: forgetting to add
mainp->msg_id_base can lead to very confusing failures.

If you stick to modifying the generated boilerplate with care -
instead of trying to build code from first principles - you'll save
yourself a bunch of time and aggravation

myplugin_test.c
---------------

This file contains binary API message **generation** code, which is
compiled into a separate .so file. The "vpp_api_test" program loads
these plugins, yielding immediate access to your plugin APIs for
external client binary API testing.

vpp itself loads test plugins, and makes the code available via the
"binary-api" debug CLI. This is a favorite way to unit-test binary
APIs prior to integration testing.

node.c
------

This is the generated graph node dispatch function. You'll need to
rewrite it to solve the problem at hand. It will save considerable
time and aggravation to retain the **structure** of the node dispatch
function.

Even for an expert, it's a waste of time to reinvent the *loop
structure*, enqueue patterns, and so forth. Simply tear out and
replace the specimen 1x, 2x, 4x packet processing code with code
relevant to the problem you're trying to solve.

myplugin.api
------------

This contains the API message definition. Here we only have defined
a single one named ``myplugin_enable_disable`` and an implicit
``myplugin_enable_disable_reply`` containing only a return value due
to the ``autoreply`` keyword.

The syntax reference for ``.api`` files can be found at VPP API Language

Addressing the binary API with this message will run the handler defined
in ``myplugin.c`` as ``vl_api_myplugin_enable_disable_t_handler``.
It will receive a message pointer ``*mp`` which is the struct defined
in ``myplugin.api`` and should return another message pointer ``*rmp``,
of the reply type. That's what ``REPLY_MACRO`` does.

To be noted, all API messages are in net-endian and vpp is host-endian,
so you will need to use :

* ``u32 value = ntohl(mp->value);``
* ``rmp->value = htonl(value);``

You can now use this API with :ref:`GoLang bindings <govpp>`

myplugin_periodic.c
-------------------

This defines a VPP process, a routine that will run indefinitely and
be woken up intermittently, here to process plugin events.

To be noted, vlib_processes aren't thread-safe, and data structures
should be locked when shared between workers.

Plugin "Friends with Benefits"
------------------------------

In vpp VLIB_INIT_FUNCTION functions, It's reasonably common to see a
specific init function invoke other init functions:

.. code-block:: C

    if ((error = vlib_call_init_function (vm, some_other_init_function))
       return error;

In the case where one plugin needs to call a init function in another
plugin, use the vlib_call_plugin_init_function macro:

.. code-block:: C

    if ((error = vlib_call_plugin_init_function (vm, "otherpluginname", some_init_function))
       return error;

This allows sequencing between plugin init functions.

If you wish to obtain a pointer to a symbol in another plugin, use the
vlib_plugin_get_symbol(...) API:

.. code-block:: C

    void *p = vlib_get_plugin_symbol ("plugin_name", "symbol");

More Examples
-------------

For more information you can read many example plugins in the directory "./src/plugins".
