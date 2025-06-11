import 'package:flutter/cupertino.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:grouped_list/grouped_list.dart';
import 'package:llms/llms.dart';
import 'package:llms_example/file_provider.dart';
import 'package:llms_example/other/message.dart';
import 'package:llms_example/other/message_role.dart';
import 'package:llms_example/other/mock_res.dart';
import 'package:markdown_widget/config/markdown_generator.dart';
import 'package:material_symbols_icons/symbols.dart';

class HomeScreen extends StatefulWidget {
  const HomeScreen({super.key});

  @override
  State<HomeScreen> createState() => _HomeScreenState();
}

class _HomeScreenState extends State<HomeScreen> {
  final GlobalKey<ScaffoldState> _scaffoldKey = GlobalKey<ScaffoldState>();
  List<Message> messageList = MockRes.mockedMessageList;

  final ScrollController _scrollController = ScrollController();
  final TextEditingController textEditingController = TextEditingController();
  final FocusNode textEditingFocusNode = FocusNode();
  final FileProvider _fileProvider = FileProvider();

  // double textFieldContainerHeight = 92.0;
  double textFieldContainerHeight = 112.0;
  String modelContextId = "";
  String filePath = "";

  @override
  void initState() {
    super.initState();
    textEditingFocusNode.addListener(_textFieldFocusNodeListener);
  }

  @override
  void dispose() {
    textEditingController.removeListener(_textFieldFocusNodeListener);
    super.dispose();
  }

  void _textFieldFocusNodeListener() {
    print('hasFocus?: ${textEditingFocusNode.hasFocus}');
    setState(() {
      if (textEditingFocusNode.hasFocus) {
        textFieldContainerHeight = 58.0;
      } else {
        textFieldContainerHeight = 112.0;
      }
    });
  }

  Future<void> _onMessageSubmit() async {
    FocusScope.of(context).requestFocus(textEditingFocusNode);
    setState(() {
      messageList.add(
        Message(
          uuid: 'asd'.trim(),
          message: textEditingController.text,
          createAt: DateTime.now(),
          messageRole: MessageRole.user,
        ),
      );
      textEditingController.clear();
      print('object');
      // _scrollToBottom();
    });
  }

  void _scrollToBottom() {
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (_scrollController.hasClients) {
        _scrollController.animateTo(
          0,
          duration: Duration(milliseconds: 600),
          curve: Curves.ease,
        );
      }
    });
  }

  void snakBarOfContext(BuildContext context, String message) {
    SnackBar snackBar = SnackBar(content: Text(message));
    ScaffoldMessenger.of(context).showSnackBar(snackBar);
  }

  Future<void> _openModelFile() async {
    String? _filePath = await _fileProvider.openGgufFile();
    if (_filePath != null) {
      setState(() {
        filePath = _filePath;
      });
      snakBarOfContext(context, 'Open sucseeful');
      Llms.instance()
          ?.initContext(_filePath, emitLoadProgress: true)
          .then((context) {
            modelContextId = context?["contextId"].toString() ?? "";
            if (modelContextId.isNotEmpty) {
              // you can get modelContextId，if modelContextId > 0 is success.
            }
          })
          .then((_) {
            Llms.instance()
                ?.completion(
                  double.parse(modelContextId),
                  prompt:
                      'This is a conversation between user and Llms. Please only output the answer and no examples needed. \n User: Привет! Как прошел твой день ? \n Llms:',
                  nPredict: 100,
                  emitRealtimeCompletion: true,
                  stop: ["<eos>", "User"],
                )
                .then((res) {
                  debugPrint("[Llms] Res=$res");
                });
            // Llms.instance()?.onTokenStream?.listen((data) {
            //   if (data['function'] == "loadProgress") {
            //     debugPrint("[FCllama] loadProgress=${data['result']}");
            //   } else if (data['function'] == "completion") {
            //     debugPrint("[FCllama] completion=${data['result']}");
            //     final tempRes = data["result"]["token"];
            //     debugPrint(tempRes);
            //     // tempRes is ans
            //   }
            // });
            // Llms.instance()
            //     ?.tokenize(
            //       double.parse(modelContextId),
            //       text: "What can you do?",
            //     )
            //     .then((res) {
            //       debugPrint("[FCllama] Tokenize Res $res");
            //       Llms.instance()
            //           ?.detokenize(
            //             double.parse(modelContextId),
            //             tokens: res?['tokens'],
            //           )
            //           .then((res) {
            //             debugPrint("[FCllama] Detokenize Res $res");
            //           });
            //     });
          });
      return;
    }
    snakBarOfContext(context, 'Error: ?');
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      key: _scaffoldKey,
      backgroundColor: Colors.white,
      resizeToAvoidBottomInset: true,
      drawer: Drawer(
        width: MediaQuery.sizeOf(context).width * 0.64,
        child: Container(),
      ),
      appBar: CupertinoNavigationBar(
        backgroundColor: Colors.white,
        leading: IconButton(
          onPressed: () {
            _scaffoldKey.currentState?.openDrawer();
          },
          icon: Icon(Symbols.notes, size: 24.0),
        ),
        middle: Text('llms example'),
        trailing: IconButton(
          onPressed: () async {
            _openModelFile();
          },
          icon: Icon(Symbols.folder, size: 24.0),
        ),
      ),
      body: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Expanded(
            child: GroupedListView<Message, DateTime>(
              reverse: true,
              elements: messageList,
              controller: _scrollController,
              groupBy: (message) => DateTime(2025),
              order: GroupedListOrder.DESC,
              sort: true,
              floatingHeader: true,
              useStickyGroupSeparators: true,
              padding: EdgeInsets.all(12.0),
              addAutomaticKeepAlives: true,
              keyboardDismissBehavior: ScrollViewKeyboardDismissBehavior.onDrag,
              groupHeaderBuilder: (Message message) => SizedBox(),
              itemBuilder: (BuildContext context, Message message) {
                if (message.messageRole == MessageRole.user) {
                  return Row(
                    mainAxisAlignment: MainAxisAlignment.end,
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Container(
                        color: Colors.white,
                        alignment: Alignment.centerRight,
                        width: MediaQuery.of(context).size.width * 0.92,
                        child: Container(
                          margin: EdgeInsets.only(left: 16.0, bottom: 22.0),
                          padding: EdgeInsets.all(9.0),
                          decoration: BoxDecoration(
                            // color: BrandColors.subTextLight,
                            color: Colors.white24,
                            borderRadius: BorderRadius.circular(12.0),
                          ),
                          child: Text(
                            message.message,
                            style: TextStyle(fontSize: 16.0),
                          ),
                        ),
                      ),
                    ],
                  );
                }
                return Container(
                  margin: EdgeInsets.only(bottom: 22.0),
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.stretch,
                    children: [
                      ...MarkdownGenerator().buildWidgets(message.message),
                      SizedBox(height: 6.0),
                      Row(
                        children: [
                          Container(
                            width: 32.0,
                            height: 32.0,
                            child: IconButton(
                              padding: EdgeInsets.all(0),
                              iconSize: 22.0,
                              icon: Icon(Symbols.content_copy),
                              onPressed: () async {
                                await Clipboard.setData(
                                  ClipboardData(
                                    text:
                                        '\n\nTokens per second: ${0.122.toStringAsFixed(2)}',
                                  ),
                                );
                                // ignore: use_build_context_synchronously
                                snakBarOfContext(
                                  context,
                                  'Copied to clipboard',
                                );
                              },
                            ),
                          ),
                          SizedBox(width: 2),
                          Text(
                            'Tokens per second: 47.32',
                            style: TextStyle(color: Colors.black54),
                          ),
                        ],
                      ),
                    ],
                  ),
                );
              }, // optional
            ),
          ),
          Container(
            // height: 58.0,
            // height: 92.0,
            height: textFieldContainerHeight,
            alignment: Alignment.topCenter,
            width: double.maxFinite,
            padding: EdgeInsets.symmetric(horizontal: 12.0, vertical: 6.0),
            decoration: BoxDecoration(
              color: Colors.white,
              // border: Border(
              //   top: BorderSide(color: Color.fromRGBO(0, 0, 0, 0.08)),
              // ),
            ),
            child: Container(
              height: 42.0,
              width: double.infinity,
              padding: EdgeInsets.symmetric(horizontal: 12.0),
              alignment: Alignment.center,
              decoration: BoxDecoration(
                color: Colors.black12,
                borderRadius: BorderRadius.circular(12.0),
                border: Border.all(color: Color.fromRGBO(0, 0, 0, 0.08)),
                boxShadow: [
                  BoxShadow(
                    color: Color.fromRGBO(0, 0, 0, 0.06),
                    offset: Offset(0, 2),
                    blurRadius: 4.0,
                    spreadRadius: 0.0,
                  ),
                ],
              ),
              child: TextField(
                maxLines: 1,
                autofocus: false,

                focusNode: textEditingFocusNode,
                controller: textEditingController,

                onTap: () => _scrollToBottom(),
                onSubmitted: (value) => _onMessageSubmit(),

                style: TextStyle(fontSize: 16.0),
                keyboardType: TextInputType.text,
                textAlignVertical: TextAlignVertical.center,

                decoration: InputDecoration(
                  filled: true,
                  border: OutlineInputBorder(
                    borderSide: BorderSide.none,
                    borderRadius: BorderRadius.all(Radius.circular(0)),
                  ),
                  fillColor: Colors.transparent,
                  contentPadding: EdgeInsets.zero,
                  floatingLabelBehavior: FloatingLabelBehavior.never,
                  suffixIconConstraints: BoxConstraints(
                    maxHeight: 38.0,
                    maxWidth: 38.0,
                  ),
                  suffixIcon: IconButton(
                    iconSize: 22.0,
                    onPressed: () => {},
                    icon: Icon(CupertinoIcons.mic),
                  ),
                  labelText: 'Message',
                  hintText: "Enter your prompt here...",
                ),
              ),
            ),
          ),
        ],
      ),
    );
  }
}
