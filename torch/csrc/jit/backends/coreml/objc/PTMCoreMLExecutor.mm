#import <torch/csrc/jit/backends/coreml/objc/PTMCoreMLExecutor.h>

#import <CoreML/CoreML.h>

@implementation PTMCoreMLExecutor {
  MLModel *_model;
  NSArray *_featureNames;
  PTMCoreMLFeatureProvider *_inputProvider;
}

- (instancetype)initWithModel:(MLModel *)model featureNames:(NSArray<NSString *> *)featureNames {
  if (self = [super init]) {
    _model = model;
    _featureNames = featureNames;

    NSSet<NSString *> *featureNamesSet = [NSSet setWithArray:featureNames];
    _inputProvider = [[PTMCoreMLFeatureProvider alloc] initWithFeatureNames:featureNamesSet];
  }
  return self;
}

- (void)setInputs:(c10::impl::GenericList)inputs {
  [_inputProvider clearInputTensors];

  int input_count = 0;
  for (int i = 0; i < inputs.size(); ++i) {
    at::IValue val = inputs.get(i);
    if (val.isTuple()) {
      auto& tuples = val.toTupleRef().elements();
      for (auto& ival : tuples) {
        [_inputProvider setInputTensor:ival.toTensor() forFeatureName:_featureNames[input_count]];
        input_count++;
      }
    } else {
      [_inputProvider setInputTensor:val.toTensor() forFeatureName:_featureNames[input_count]];
      input_count++;
    }
  }
}

- (id<MLFeatureProvider>)forward {
  NSError *error;
  MLPredictionOptions *options = [[MLPredictionOptions alloc] init];
  id<MLFeatureProvider> outputs = [_model predictionFromFeatures:_inputProvider options:options error:&error];
  if (error) {
    NSLog(@"Prediction failed with error %@", error);
    return nil;
  }
  return outputs;
}

@end
